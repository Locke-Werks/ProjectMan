#include "enrich.h"

#include "discovery.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <system_error>
#include <vector>

namespace pm {
namespace {

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        if (raw)
            CoTaskMemFree(raw);
        return {};
    }
    fs::path out(raw);
    CoTaskMemFree(raw);
    return out;
}

fs::path claudeHome()
{
    const fs::path profile = knownFolder(FOLDERID_Profile);
    return profile.empty() ? fs::path{} : profile / ".claude";
}

// ------------------------------------------------------------ tiny JSON bits
//
// These records are machine-written one-per-line objects with known keys. A
// full parser would be a dependency and a lot of allocation per line across
// 3,000+ records, for values that are always a flat string or number.

// Value of "key":"..." with the standard escapes undone. Empty if absent.
std::string jsonString(std::string_view line, std::string_view key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\":\"";

    const size_t at = line.find(needle);
    if (at == std::string_view::npos)
        return {};

    size_t i = at + needle.size();
    std::string out;
    out.reserve(64);

    while (i < line.size()) {
        const char c = line[i];
        if (c == '"')
            break;
        if (c == '\\' && i + 1 < line.size()) {
            const char e = line[++i];
            switch (e) {
            case 'n':  out.push_back('\n'); break;
            case 't':  out.push_back('\t'); break;
            case 'r':  break;   // dropped: these end up in one-line summaries
            case 'u':
                // Not decoded. No key read here carries one in practice, and a
                // mangled display string is better than a wrong byte count.
                i += 4;
                break;
            default:   out.push_back(e); break;
            }
            ++i;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::int64_t jsonNumber(std::string_view line, std::string_view key)
{
    std::string needle = "\"";
    needle += key;
    needle += "\":";

    const size_t at = line.find(needle);
    if (at == std::string_view::npos)
        return 0;

    size_t i = at + needle.size();
    while (i < line.size() && line[i] == ' ')
        ++i;

    std::string digits;
    while (i < line.size() && (std::isdigit(static_cast<unsigned char>(line[i]))))
        digits.push_back(line[i++]);

    return digits.empty() ? 0 : std::atoll(digits.c_str());
}

std::string keyForPath(const fs::path& p)
{
    std::string s = toLower(p.string());
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.back() == '\\')
        s.pop_back();
    return s;
}

// Every character outside [A-Za-z0-9-] becomes a single '-'. Verified against
// the real directories: "Coding Compendium" is Coding-Compendium and
// "HHS ATO-392" is HHS-ATO-392. The transform is lossy, so it is only ever
// applied forward; the reverse cannot be recovered.
std::string slugForPath(const fs::path& p)
{
    const std::string s = p.string();
    std::string out;
    out.reserve(s.size());
    for (const char c : s) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                       || (c >= '0' && c <= '9') || c == '-';
        out.push_back(keep ? c : '-');
    }
    return out;
}

// The last few KB of a transcript, which is where ai-title and last-prompt sit.
// Sessions reach 856 KB and reading one whole per project is pointless.
std::string tailOf(const fs::path& file, std::streamoff bytes)
{
    std::ifstream f(file, std::ios::binary);
    if (!f)
        return {};

    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    const std::streamoff from = size > bytes ? size - bytes : 0;
    f.seekg(from, std::ios::beg);

    std::string out((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return out;
}

struct HistoryEntry {
    int          prompts = 0;
    std::int64_t lastMs  = 0;
};

} // namespace

void enrichClaudeState(ProjectList& projects, const Config&)
{
    const fs::path home = claudeHome();
    if (home.empty())
        return;

    // One pass over history.jsonl. Each record carries the project path
    // unmangled, so this needs no slug work at all.
    std::map<std::string, HistoryEntry> byPath;
    {
        std::ifstream f(home / "history.jsonl");
        std::string   line;
        while (std::getline(f, line)) {
            if (line.empty())
                continue;
            const std::string project = jsonString(line, "project");
            if (project.empty())
                continue;

            HistoryEntry& e = byPath[keyForPath(fs::path(widen(project)))];
            ++e.prompts;
            const std::int64_t ts = jsonNumber(line, "timestamp");
            if (ts > e.lastMs)
                e.lastMs = ts;
        }
    }

    const fs::path projectsDir = home / "projects";

    for (Project& p : projects) {
        const auto it = byPath.find(keyForPath(p.path));
        if (it != byPath.end()) {
            p.open.claudePrompts = it->second.prompts;
            p.open.claudeLastUnix = it->second.lastMs / 1000;
        }

        std::error_code ec;
        if (!fs::is_directory(projectsDir, ec))
            continue;

        // The slug's drive letter is not case-normalised (one directory is
        // "c--Users-..."), so the match has to be case-insensitive.
        const std::string want = slugForPath(p.path);
        fs::path          stateDir;
        for (const auto& e : fs::directory_iterator(projectsDir, ec)) {
            if (ec)
                break;
            if (iequals(e.path().filename().string(), want)) {
                stateDir = e.path();
                break;
            }
        }
        if (stateDir.empty())
            continue;

        fs::path      newest;
        std::int64_t  newestTime = 0;
        int           sessions   = 0;
        for (const auto& e : fs::directory_iterator(stateDir, ec)) {
            if (ec)
                break;
            if (e.path().extension() != ".jsonl")
                continue;
            ++sessions;
            const auto t = fs::last_write_time(e.path(), ec);
            if (ec)
                continue;
            const auto v = t.time_since_epoch().count();
            if (v > newestTime) {
                newestTime = v;
                newest     = e.path();
            }
        }
        p.open.claudeSessions = sessions;
        if (newest.empty())
            continue;

        // Scan the tail backwards: the newest record of each kind wins.
        const std::string tail = tailOf(newest, 96 * 1024);
        const auto        lines = splitLines(tail);
        for (auto it2 = lines.rbegin(); it2 != lines.rend(); ++it2) {
            if (p.open.claudeTitle.empty()
                && it2->find("\"type\":\"ai-title\"") != std::string_view::npos) {
                p.open.claudeTitle = jsonString(*it2, "aiTitle");
            }
            if (p.open.claudeLastPrompt.empty()
                && it2->find("\"type\":\"last-prompt\"") != std::string_view::npos) {
                p.open.claudeLastPrompt = jsonString(*it2, "lastPrompt");
            }
            if (!p.open.claudeTitle.empty() && !p.open.claudeLastPrompt.empty())
                break;
        }

        // A prompt that spans lines is unreadable in a single-line summary.
        for (std::string* s : { &p.open.claudeTitle, &p.open.claudeLastPrompt }) {
            const size_t nl = s->find('\n');
            if (nl != std::string::npos)
                s->resize(nl);
            if (s->size() > 160)
                s->resize(160);
        }
    }
}

void enrichChecklists(ProjectList& projects, const Config& cfg)
{
    for (Project& p : projects) {
        if (p.kind == ProjectKind::BareRepo)
            continue;

        int  unchecked = 0;
        int  filesSeen = 0;
        std::error_code ec;

        fs::recursive_directory_iterator it(
            p.path, fs::directory_options::skip_permission_denied, ec);
        if (ec)
            continue;

        const fs::recursive_directory_iterator end;
        while (it != end) {
            // A guard, not a limit anyone should hit: a project with thousands
            // of markdown files is a vendored tree the prune list missed, and
            // counting all of it would stall the pass.
            if (filesSeen > 4000)
                break;

            const fs::path entry = it->path();
            const std::string name = entry.filename().string();

            std::error_code isDirEc;
            if (it->is_directory(isDirEc) && !isDirEc) {
                // .claude holds agent and skill templates, which are markdown
                // full of unchecked boxes belonging to the template rather than
                // to the project. In one fork that is 225 boxes against 401
                // real ones.
                const bool skip = isPruned(name) || name == ".github"
                               || (!name.empty() && name.front() == '.');

                // A nested repository is its own row in the list, so counting
                // it here would report its work twice: once against the
                // container and once against itself. HHS General Ops holds four
                // such checkouts.
                std::error_code nestedEc;
                const bool nestedRepo = fs::exists(entry / ".git", nestedEc);

                if (skip || nestedRepo)
                    it.disable_recursion_pending();
            } else if (entry.extension() == ".md" || entry.extension() == ".markdown") {
                ++filesSeen;
                std::ifstream f(entry);
                std::string   line;
                while (std::getline(f, line)) {
                    const std::string_view t = trim(line);
                    if (t.starts_with("- [ ]") || t.starts_with("* [ ]")
                        || t.starts_with("- [] ") || t.starts_with("+ [ ]")) {
                        ++unchecked;
                    }
                }
            }

            it.increment(ec);
            if (ec)
                break;
        }

        p.open.checklistItems = unchecked;
    }
    (void)cfg;
}

fs::path githubCachePath()
{
    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        return {};
    return local / "ProjectMan" / "github.tsv";
}

bool enrichGitHub(ProjectList& projects, const Config& cfg, bool force,
                  GitHubTotals* totals, std::string* error)
{
    if (!cfg.githubEnabled || cfg.githubOwners.empty())
        return true;

    const fs::path cache = githubCachePath();
    std::error_code ec;

    bool fresh = false;
    if (!force && !cache.empty() && fs::exists(cache, ec)) {
        const auto age = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::file_clock::now() - fs::last_write_time(cache, ec));
        fresh = !ec && age.count() < cfg.githubCacheMinutes;
    }

    std::string body;

    if (fresh) {
        std::ifstream f(cache);
        body.assign((std::istreambuf_iterator<char>(f)),
                    std::istreambuf_iterator<char>());
    } else {
        wchar_t     buf[MAX_PATH * 2] = {};
        const DWORD n = SearchPathW(nullptr, L"gh.exe", nullptr,
                                    static_cast<DWORD>(std::size(buf)), buf, nullptr);
        if (n == 0 || n >= std::size(buf)) {
            if (error)
                *error = "gh not found on PATH";
            return false;
        }
        const fs::path gh(std::wstring(buf, n));

        // Two searches over the whole owner set, not one call per repository.
        // There are only about 15 open issues and 14 open pull requests across
        // all four owners, so 60-odd `gh issue list` invocations would be
        // almost entirely empty round trips.
        //
        // Both are needed: `gh search issues` returns issues only, and reports
        // isPullRequest false on every result, so it can never find a PR.
        //
        // --jq shapes each answer into TSV, so nothing here parses JSON.
        for (const char* kind : { "issues", "prs" }) {
            std::vector<std::string> args = { "search", kind, "--state", "open",
                                              "--limit", "500" };
            for (const std::string& owner : cfg.githubOwners) {
                args.emplace_back("--owner");
                args.push_back(owner);
            }
            args.emplace_back("--json");
            args.emplace_back("repository");
            args.emplace_back("--jq");
            args.emplace_back(std::string(".[] | [.repository.nameWithOwner, \"")
                              + (std::string(kind) == "prs" ? "pr" : "issue")
                              + "\"] | @tsv");

            const ProcResult r = run(gh, args, {}, 60000);
            if (!r.started) {
                if (error)
                    *error = "could not run gh: " + r.launchError;
                return false;
            }
            if (r.exitCode != 0) {
                if (error)
                    *error = std::string(trim(r.err));
                return false;
            }
            body += r.out;
            if (!body.empty() && body.back() != '\n')
                body.push_back('\n');
        }
        if (!cache.empty()) {
            fs::create_directories(cache.parent_path(), ec);
            std::ofstream f(cache, std::ios::binary | std::ios::trunc);
            f << body;
        }
    }

    struct Counts {
        int prs = 0, issues = 0;
    };
    std::map<std::string, Counts> byRepo;

    for (const std::string_view line : splitLines(body)) {
        const std::string_view t = trim(line);
        if (t.empty())
            continue;
        const size_t tab = t.find('\t');
        if (tab == std::string_view::npos)
            continue;

        Counts& c = byRepo[toLower(t.substr(0, tab))];
        if (trim(t.substr(tab + 1)) == "pr")
            ++c.prs;
        else
            ++c.issues;
    }

    // Distinct, straight from the fetch. Summing the projects would count a
    // repository once per checkout, and several here are checked out two or
    // three times.
    if (totals) {
        totals->repos = static_cast<int>(byRepo.size());
        for (const auto& [name, c] : byRepo) {
            totals->prs    += c.prs;
            totals->issues += c.issues;
        }
    }

    for (Project& p : projects) {
        if (p.ownerRepo.empty())
            continue;
        const auto it = byRepo.find(toLower(p.ownerRepo));
        if (it == byRepo.end())
            continue;
        p.open.openPrs    = it->second.prs;
        p.open.openIssues = it->second.issues;
    }

    return true;
}

} // namespace pm
