#include "git.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>

#include <cstdlib>
#include <fstream>
#include <mutex>
#include <system_error>

namespace pm::git {
namespace {

// Every invocation carries this.
//
// `git status` is not read-only: it refreshes and rewrites .git/index, taking
// index.lock to do so. Firing sixty of those at repos where an editor or a live
// Claude session is working produces spurious "Unable to create index.lock"
// failures in the user's other tools. This is correctness, not tuning.
std::vector<std::string> argsFor(std::initializer_list<const char*> extra)
{
    std::vector<std::string> a = { "--no-optional-locks" };
    for (const char* e : extra)
        a.emplace_back(e);
    return a;
}

fs::path       g_override;
fs::path       g_git;
std::once_flag g_resolveOnce;

void resolve()
{
    if (!g_override.empty()) {
        g_git = g_override;
        return;
    }

    // SearchPathW honours this process's real PATH, including anything a shell
    // prepended, which splitting getenv by hand does not reliably do.
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = SearchPathW(nullptr, L"git.exe", nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf)) {
        g_git = fs::path(std::wstring(buf, n));
        return;
    }

    for (const wchar_t* candidate : { L"C:\\Program Files\\Git\\cmd\\git.exe",
                                      L"C:\\Program Files (x86)\\Git\\cmd\\git.exe" }) {
        std::error_code ec;
        if (fs::exists(candidate, ec)) {
            g_git = candidate;
            return;
        }
    }
}

bool looksBare(const fs::path& dir)
{
    std::error_code ec;
    return fs::exists(dir / "HEAD", ec) && fs::is_directory(dir / "objects", ec)
        && fs::is_directory(dir / "refs", ec);
}

} // namespace

void setExePathOverride(const fs::path& p) { g_override = p; }

const fs::path& exePath()
{
    std::call_once(g_resolveOnce, resolve);
    return g_git;
}

ProjectKind classify(const fs::path& dir)
{
    std::error_code ec;

    // A linked worktree stores .git as a FILE containing "gitdir: ...", so
    // testing for a directory is wrong. exists() covers both forms.
    if (fs::exists(dir / ".git", ec))
        return ProjectKind::Repo;

    // Detected by inspection rather than by asking git, so the bare mirror
    // costs no process and never produces "must be run in a work tree".
    if (dir.extension() == ".git" || looksBare(dir))
        return ProjectKind::BareRepo;

    return ProjectKind::Plain;
}

RepoProbe probe(const fs::path& dir)
{
    RepoProbe out;
    std::error_code ec;

    const fs::path dotGit = dir / ".git";

    // Resolved from the filesystem rather than by spawning
    // `git rev-parse --absolute-git-dir`. That call is correct but costs a
    // process per repository, and across this tree that was a third of the
    // whole sweep for an answer three lines of file reading already have.
    if (fs::is_directory(dotGit, ec)) {
        out.isRepo = true;
        out.gitDir = dotGit;
        return out;
    }

    if (fs::is_regular_file(dotGit, ec)) {
        // A linked worktree: the file holds "gitdir: <path>".
        std::ifstream f(dotGit);
        std::string   line;
        if (f && std::getline(f, line)) {
            const std::string_view t = trim(line);
            if (istartsWith(t, "gitdir:")) {
                fs::path g(widen(trim(t.substr(7))));
                if (g.is_relative())
                    g = dir / g;

                // <worktree gitdir>/commondir points at the shared .git, which
                // is where config and therefore the origin URL actually live.
                std::ifstream cf(g / "commondir");
                std::string   common;
                if (cf && std::getline(cf, common)) {
                    fs::path c(widen(trim(common)));
                    g = c.is_relative() ? (g / c) : c;
                }

                out.isRepo = true;
                out.gitDir = fs::weakly_canonical(g, ec);
                if (ec)
                    out.gitDir = g;
                return out;
            }
        }
        return out;
    }

    // A bare repository is its own git directory.
    if (dir.extension() == ".git" || looksBare(dir)) {
        out.isRepo = true;
        out.isBare = true;
        out.gitDir = dir;
    }
    return out;
}

GitStatus parsePorcelainV2(std::string_view text)
{
    GitStatus st;

    for (const std::string_view raw : splitLines(text)) {
        if (raw.empty())
            continue;

        if (raw.starts_with("# branch.oid ")) {
            st.headOid = std::string(trim(raw.substr(13)));
        } else if (raw.starts_with("# branch.head ")) {
            st.branch = std::string(trim(raw.substr(14)));
        } else if (raw.starts_with("# branch.upstream ")) {
            st.upstream    = std::string(trim(raw.substr(18)));
            st.hasUpstream = true;
        } else if (raw.starts_with("# branch.ab ")) {
            // "+3 -0". Absent entirely when there is no upstream, which is why
            // hasUpstream is tracked rather than inferred from a zero here.
            const std::string_view ab = trim(raw.substr(12));
            const size_t           sp = ab.find(' ');
            if (sp != std::string_view::npos && sp + 2 <= ab.size()) {
                st.ahead  = std::atoi(std::string(ab.substr(1, sp - 1)).c_str());
                st.behind = std::atoi(std::string(ab.substr(sp + 2)).c_str());
            }
        } else if (raw.starts_with("# stash ")) {
            st.stashes = std::atoi(std::string(trim(raw.substr(8))).c_str());
        } else if (raw.front() == '?') {
            ++st.untracked;
        } else if (raw.front() == 'u') {
            ++st.conflicted;
        } else if (raw.front() == '1' || raw.front() == '2') {
            // "1 XY ...", where X is the index state and Y the work-tree state,
            // '.' meaning unchanged. A file can be both, and git's own summary
            // counts it in both columns, so this does too. The '2' form carries
            // an extra tab-separated original path, which changes nothing here
            // because only the fixed-offset XY field is read.
            if (raw.size() >= 4) {
                if (raw[2] != '.')
                    ++st.staged;
                if (raw[3] != '.')
                    ++st.unstaged;
            }
        }
    }

    st.state = st.dirty() ? GitState::Dirty : GitState::Clean;
    return st;
}

GitStatus status(const fs::path& workdir, const RepoProbe& p, const ProbeOptions& opt)
{
    GitStatus st;

    if (!p.isRepo) {
        st.state = GitState::NotARepo;
        return st;
    }

    // A bare repository answers rev-parse but has no work tree: status exits
    // with "fatal: this operation must be run in a work tree". Short-circuiting
    // saves a spawn and avoids a guaranteed error.
    if (p.isBare) {
        st.state  = GitState::Bare;
        st.branch = "(bare)";
        return st;
    }

    const fs::path& git = exePath();
    if (git.empty()) {
        st.state = GitState::Error;
        st.error = "git.exe not found";
        return st;
    }

    const ProcResult r =
        run(git, argsFor({ "status", "--porcelain=v2", "--branch", "--show-stash" }),
            workdir, opt.timeoutMs);

    if (!r.started) {
        st.state = GitState::Error;
        st.error = r.launchError;
        return st;
    }
    if (r.timedOut) {
        st.state = GitState::Error;
        st.error = "git status timed out";
        return st;
    }
    if (r.exitCode != 0) {
        st.state = GitState::Error;
        st.error = std::string(trim(r.err));
        return st;
    }

    st           = parsePorcelainV2(r.out);
    st.elapsedMs = r.elapsedMs;

    if (opt.wantLastCommit) {
        // Unit separators, because a subject line can contain anything.
        const ProcResult log =
            run(git, argsFor({ "log", "-1", "--format=%H%x1f%ct%x1f%an%x1f%s" }),
                workdir, 10000);

        if (log.started && log.exitCode == 0) {
            const std::string out(trim(log.out));
            const size_t      a = out.find('\x1f');
            const size_t      b = (a == std::string::npos) ? a : out.find('\x1f', a + 1);
            const size_t      c = (b == std::string::npos) ? b : out.find('\x1f', b + 1);
            if (c != std::string::npos) {
                st.lastCommitUnix    = std::atoll(out.substr(a + 1, b - a - 1).c_str());
                st.lastCommitAuthor  = out.substr(b + 1, c - b - 1);
                st.lastCommitSubject = std::string(trim(out.substr(c + 1)));
            }
        }
        // A repository with no commits yet fails that call. Leaving the fields
        // empty is the correct answer, not an error.
        st.elapsedMs += log.elapsedMs;
    }

    return st;
}

std::string originUrl(const fs::path& workdir, const fs::path& gitDir)
{
    // <gitdir>/config is a stable INI. Reading it directly saves one process
    // spawn per repo, which is ~50 ms times every repo in the tree.
    if (!gitDir.empty()) {
        std::ifstream f(gitDir / "config");
        if (f) {
            std::string line;
            bool        inOrigin = false;
            while (std::getline(f, line)) {
                const std::string_view t = trim(line);
                if (!t.empty() && t.front() == '[') {
                    inOrigin = t.starts_with("[remote \"origin\"]");
                    continue;
                }
                if (!inOrigin || !istartsWith(t, "url"))
                    continue;
                const size_t eq = t.find('=');
                if (eq != std::string_view::npos)
                    return std::string(trim(t.substr(eq + 1)));
            }
        }
    }

    // Conditional includes and worktree-specific config can put the remote
    // somewhere a plain file read will not find it.
    const fs::path& git = exePath();
    if (git.empty())
        return {};

    const ProcResult r =
        run(git, argsFor({ "config", "--get", "remote.origin.url" }), workdir, 10000);
    return r.exitCode == 0 ? std::string(trim(r.out)) : std::string();
}

std::string ownerRepoFromUrl(std::string_view url)
{
    std::string_view s = trim(url);
    if (s.empty())
        return {};

    // Strip the scheme and any user prefix, leaving "github.com[:/]owner/repo".
    for (const std::string_view prefix :
         { "https://", "http://", "ssh://git@", "git://", "git@" }) {
        if (istartsWith(s, prefix)) {
            s.remove_prefix(prefix.size());
            break;
        }
    }

    if (!istartsWith(s, "github.com"))
        return {};
    s.remove_prefix(std::string_view("github.com").size());

    while (!s.empty() && (s.front() == ':' || s.front() == '/'))
        s.remove_prefix(1);

    if (s.ends_with("/"))
        s.remove_suffix(1);
    if (s.ends_with(".git"))
        s.remove_suffix(4);

    const size_t slash = s.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 >= s.size())
        return {};

    return std::string(s);
}

int countWorktrees(const fs::path& gitDir)
{
    if (gitDir.empty())
        return 0;

    std::error_code ec;
    const fs::path  wt = gitDir / "worktrees";
    if (!fs::is_directory(wt, ec))
        return 0;

    int n = 0;
    for (const auto& e : fs::directory_iterator(wt, ec)) {
        if (ec)
            break;
        if (e.is_directory(ec))
            ++n;
    }
    return n;
}

namespace {

// XY from porcelain v1. X is the index, Y is the work tree, and the work tree
// wins for the label because it is the change a person has not dealt with yet.
std::string statusLabel(std::string_view code)
{
    if (code == "??")
        return "untracked";
    if (code == "!!")
        return "ignored";

    const char x = code.size() > 0 ? code[0] : ' ';
    const char y = code.size() > 1 ? code[1] : ' ';

    // Both sides lettered with the same letter, or either side U, is a conflict.
    if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D'))
        return "conflicted";

    const char c = (y != ' ') ? y : x;
    switch (c) {
    case 'M': return "modified";
    case 'A': return "added";
    case 'D': return "deleted";
    case 'R': return "renamed";
    case 'C': return "copied";
    case 'T': return "typechange";
    default:  return "changed";
    }
}

// porcelain v1 with -z: "XY<space><path>" per NUL-terminated record, and a
// rename or copy carries its source path as the very next record. -z is used
// rather than the default so no path is ever quoted or escaped, which is the
// whole class of bug that eats non-ASCII and spaces.
void parseStatusZ(std::string_view text, int cap, std::vector<ChangedFile>* out,
                  int* total)
{
    std::vector<std::string_view> records;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t nul = text.find('\0', start);
        if (nul == std::string_view::npos) {
            if (start < text.size())
                records.push_back(text.substr(start));
            break;
        }
        records.push_back(text.substr(start, nul - start));
        start = nul + 1;
    }

    for (size_t i = 0; i < records.size(); ++i) {
        const std::string_view rec = records[i];
        if (rec.size() < 4)
            continue;

        ChangedFile f;
        f.code  = std::string(rec.substr(0, 2));
        f.path  = std::string(rec.substr(3));
        f.label = statusLabel(f.code);
        f.staged = f.code[0] != ' ' && f.code != "??";

        // The source of a rename or copy is its own record and is not a change
        // in its own right.
        if (f.code[0] == 'R' || f.code[0] == 'C')
            ++i;

        ++*total;
        if (cap <= 0 || static_cast<int>(out->size()) < cap)
            out->push_back(std::move(f));
    }
}

} // namespace

RepoDetail detail(const fs::path& workdir, const RepoProbe& p, int cap,
                  const ProbeOptions& opt)
{
    RepoDetail d;

    if (!p.isRepo || p.isBare)
        return d;

    const fs::path& git = exePath();
    if (git.empty()) {
        d.error = "git.exe not found";
        return d;
    }

    const ProcResult st =
        run(git, argsFor({ "status", "--porcelain", "-z" }), workdir, opt.timeoutMs);
    if (!st.started || st.timedOut || st.exitCode != 0) {
        d.error = st.started ? std::string(trim(st.err)) : st.launchError;
        if (d.error.empty())
            d.error = "git status failed";
        return d;
    }
    parseStatusZ(st.out, cap, &d.files, &d.filesTotal);

    // Line counts for tracked changes. Untracked files are absent from a diff
    // by definition, so they keep their zeroes rather than being invented.
    const ProcResult ns =
        run(git, argsFor({ "diff", "--numstat", "HEAD" }), workdir, opt.timeoutMs);
    if (ns.started && !ns.timedOut && ns.exitCode == 0) {
        for (const std::string_view line : splitLines(ns.out)) {
            const size_t t1 = line.find('\t');
            if (t1 == std::string_view::npos)
                continue;
            const size_t t2 = line.find('\t', t1 + 1);
            if (t2 == std::string_view::npos)
                continue;

            const std::string_view a  = line.substr(0, t1);
            const std::string_view r  = line.substr(t1 + 1, t2 - t1 - 1);
            const std::string      fp = std::string(line.substr(t2 + 1));

            // "-" on both counts is git's way of saying binary.
            const bool binary = (a == "-" || r == "-");
            const int  added   = binary ? 0 : std::atoi(std::string(a).c_str());
            const int  removed = binary ? 0 : std::atoi(std::string(r).c_str());

            d.added   += added;
            d.removed += removed;

            for (ChangedFile& f : d.files) {
                if (f.path == fp) {
                    f.added   = added;
                    f.removed = removed;
                    f.binary  = binary;
                    break;
                }
            }
        }
    }

    const ProcResult lg =
        run(git, argsFor({ "log", "-n", "8", "--format=%h%x1f%ct%x1f%an%x1f%s" }),
            workdir, opt.timeoutMs);
    if (lg.started && !lg.timedOut && lg.exitCode == 0) {
        for (const std::string_view line : splitLines(lg.out)) {
            // Unit separators, because a subject line can contain anything a
            // person felt like typing, tabs and pipes included.
            std::vector<std::string_view> f;
            size_t start = 0;
            for (;;) {
                const size_t sep = line.find('\x1f', start);
                if (sep == std::string_view::npos) {
                    f.push_back(line.substr(start));
                    break;
                }
                f.push_back(line.substr(start, sep - start));
                start = sep + 1;
            }
            if (f.size() < 4)
                continue;

            Commit c;
            c.shortOid = std::string(f[0]);
            c.when     = std::atoll(std::string(f[1]).c_str());
            c.author   = std::string(f[2]);
            c.subject  = std::string(f[3]);
            d.recent.push_back(std::move(c));
        }
    }

    return d;
}

} // namespace pm::git
