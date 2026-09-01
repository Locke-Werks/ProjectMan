#include "config.h"
#include "console.h"
#include "discovery.h"
#include "git.h"
#include "launcher.h"
#include "scanner.h"
#include "strutil.h"
#include "tui.h"
#include "workitems.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {

using namespace pm;

// Exit codes. Anything a child returns is passed straight through, so these
// only cover the cases where nothing was launched.
constexpr int kOk         = 0;
constexpr int kError      = 1;
constexpr int kUsage      = 2;
constexpr int kEnvironment = 3;
constexpr int kLaunchFail = 4;
constexpr int kNoMatch    = 5;

int fail(const std::string& msg)
{
    std::fprintf(stderr, "projectman: %s\n", msg.c_str());
    return kError;
}

int failWith(int code, const std::string& msg)
{
    std::fprintf(stderr, "projectman: %s\n", msg.c_str());
    return code;
}

void printUsage()
{
    std::fputs(
        "ProjectMan " PM_VERSION_STRING " - control surface over your projects tree\n"
        "\n"
        "Usage:\n"
        "  pm                          browse and launch Claude Code in a project\n"
        "  pm ls [options]             list projects\n"
        "  pm status <name>            one project in detail\n"
        "  pm go <name> [options]      hand this terminal to Claude Code there\n"
        "  pm open <name>              open a new terminal window there\n"
        "  pm items [--json]           every outstanding item across the tree\n"
        "  pm dispatch [options]       run one Claude Code session across repos\n"
        "  pm doctor                   resolve git, claude and wt, and time a sweep\n"
        "  pm config [--path|--init]   show, locate or write the config file\n"
        "\n"
        "Options:\n"
        "  --dirty                     only projects with uncommitted changes\n"
        "  --repos                     only git repositories\n"
        "  --sort <recent|name|dirty|open>\n"
        "  --json                      machine-readable, one object per line\n"
        "  --continue                  resume the most recent conversation there\n"
        "  --resume [id]               resume a specific session, or pick one\n"
        "  --model <alias>             fable, opus or sonnet\n"
        "  --all                       dispatch: select every outstanding item\n"
        "  --dry-run                   dispatch: print the briefing, launch nothing\n"
        "  --root <path>               override the projects root\n"
        "  -h, --help                  this text\n"
        "  -V, --version               version only\n",
        stderr);
}

struct Args {
    std::string              verb;
    std::vector<std::string> positional;
    bool        dirty = false, reposOnly = false, json = false, all = false;
    bool        dryRun = false, wantPath = false, wantInit = false;
    bool        cont = false, resume = false, help = false, version = false;
    std::string sort, model, resumeId, root;
    std::string unknown;
};

std::string valueFor(const std::vector<std::string>& v, size_t& i, const char* flag,
                     std::string* err)
{
    if (i + 1 >= v.size()) {
        *err = std::string("missing value for ") + flag;
        return {};
    }
    return v[++i];
}

Args parseArgs(const std::vector<std::string>& v, std::string* err)
{
    Args a;
    for (size_t i = 0; i < v.size(); ++i) {
        const std::string& s = v[i];

        if (s == "-h" || s == "--help")            { a.help = true; continue; }
        if (s == "-V" || s == "--version")         { a.version = true; continue; }
        if (s == "--dirty")                        { a.dirty = true; continue; }
        if (s == "--repos")                        { a.reposOnly = true; continue; }
        if (s == "--json")                         { a.json = true; continue; }
        if (s == "--all")                          { a.all = true; continue; }
        if (s == "--dry-run")                      { a.dryRun = true; continue; }
        if (s == "--path")                         { a.wantPath = true; continue; }
        if (s == "--init")                         { a.wantInit = true; continue; }
        if (s == "-c" || s == "--continue")        { a.cont = true; continue; }
        if (s == "--sort")  { a.sort  = valueFor(v, i, "--sort", err);  continue; }
        if (s == "--model") { a.model = valueFor(v, i, "--model", err); continue; }
        if (s == "--root")  { a.root  = valueFor(v, i, "--root", err);  continue; }

        if (s == "-r" || s == "--resume") {
            a.resume = true;
            // The id is optional, so only take the next token when it is not
            // itself a flag.
            if (i + 1 < v.size() && v[i + 1].rfind('-', 0) != 0)
                a.resumeId = v[++i];
            continue;
        }

        if (!s.empty() && s[0] == '-') {
            a.unknown = s;
            continue;
        }

        if (a.verb.empty())
            a.verb = s;
        else
            a.positional.push_back(s);
    }
    return a;
}

std::string relativeTime(std::int64_t unix)
{
    if (unix <= 0)
        return "-";

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    std::int64_t d = now - unix;
    if (d < 0)
        d = 0;

    if (d < 60)          return std::to_string(d) + "s";
    if (d < 3600)        return std::to_string(d / 60) + "m";
    if (d < 86400)       return std::to_string(d / 3600) + "h";
    if (d < 86400 * 30)  return std::to_string(d / 86400) + "d";
    if (d < 86400 * 365) return std::to_string(d / (86400 * 30)) + "mo";
    return std::to_string(d / (86400 * 365)) + "y";
}

std::string kindWord(ProjectKind k)
{
    switch (k) {
    case ProjectKind::Repo:      return "repo";
    case ProjectKind::BareRepo:  return "bare";
    case ProjectKind::Container: return "container";
    case ProjectKind::Plain:     return "folder";
    }
    return "?";
}

std::string stateWord(const GitStatus& g)
{
    switch (g.state) {
    case GitState::Clean:    return "clean";
    case GitState::Dirty:    return std::to_string(g.dirtyCount()) + " dirty";
    case GitState::Bare:     return "bare";
    case GitState::Error:    return "error";
    case GitState::NotARepo: return "-";
    case GitState::Unknown:  return "?";
    }
    return "?";
}

std::string syncWord(const GitStatus& g)
{
    if (g.state == GitState::NotARepo || g.state == GitState::Bare)
        return "-";
    if (!g.hasUpstream)
        return "no upstream";

    std::string s;
    if (g.ahead)
        s += "+" + std::to_string(g.ahead);
    if (g.behind)
        s += (s.empty() ? "" : "/") + std::string("-") + std::to_string(g.behind);
    return s.empty() ? "synced" : s;
}

// A silent sink: the non-interactive verbs only need the finished list.
class NullSink : public StatusSink {
public:
    void onStatus(std::size_t, const Project&) override {}
    void onScanFinished(int probed, int failed, double seconds) override
    {
        probed_ = probed;
        failed_ = failed;
        seconds_ = seconds;
    }
    int    probed_ = 0, failed_ = 0;
    double seconds_ = 0.0;
};

ProjectList scanAll(const Config& cfg, NullSink& sink)
{
    DiscoveryOptions opts;
    opts.root              = cfg.root;
    opts.descendContainers = cfg.descendContainers;
    opts.includePlain      = cfg.includePlain;
    opts.exclude           = cfg.exclude;

    std::string err;
    ProjectList projects = discover(opts, &err);

    git::ProbeOptions po;
    po.timeoutMs = cfg.probeTimeoutMs;

    CancelToken token;
    ScanPool    pool(cfg.scanThreads, po);
    pool.start(projects, sink, token);
    pool.wait();

    return projects;
}

void sortProjects(ProjectList& v, const std::string& how)
{
    if (iequals(how, "name")) {
        std::sort(v.begin(), v.end(), [](const Project& a, const Project& b) {
            return toLower(a.name) < toLower(b.name);
        });
    } else if (iequals(how, "dirty")) {
        std::sort(v.begin(), v.end(), [](const Project& a, const Project& b) {
            return a.git.dirtyCount() > b.git.dirtyCount();
        });
    } else if (iequals(how, "open")) {
        std::sort(v.begin(), v.end(), [](const Project& a, const Project& b) {
            return a.open.total() > b.open.total();
        });
    } else {   // recent
        std::sort(v.begin(), v.end(), [](const Project& a, const Project& b) {
            return a.activityUnix() > b.activityUnix();
        });
    }
}

std::string jsonEscape(std::string_view s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void printProjectJson(const Project& p)
{
    std::printf(
        "{\"name\":\"%s\",\"display\":\"%s\",\"path\":\"%s\",\"kind\":\"%s\",\"branch\":\"%s\","
        "\"upstream\":\"%s\",\"ahead\":%d,\"behind\":%d,\"dirty\":%d,\"staged\":%d,"
        "\"unstaged\":%d,\"untracked\":%d,\"stashes\":%d,\"repo\":\"%s\","
        "\"last_commit\":%lld,\"subject\":\"%s\",\"open\":%d}\n",
        jsonEscape(p.name).c_str(), jsonEscape(p.displayName()).c_str(),
        jsonEscape(p.path.string()).c_str(),
        kindWord(p.kind).c_str(), jsonEscape(p.git.branch).c_str(),
        jsonEscape(p.git.upstream).c_str(), p.git.ahead, p.git.behind,
        p.git.dirtyCount(), p.git.staged, p.git.unstaged, p.git.untracked,
        p.git.stashes, jsonEscape(p.ownerRepo).c_str(),
        static_cast<long long>(p.git.lastCommitUnix),
        jsonEscape(p.git.lastCommitSubject).c_str(), p.open.total());
}

// Exact name, then unique case-insensitive prefix, then unique substring.
// Anything containing a slash is treated as a path instead.
const Project* resolveOne(const ProjectList& v, const std::string& query,
                          std::string* err)
{
    // A forward slash is also the container separator in a display name
    // ("HHS Matrix/ocio-ato-modernization-site"), so a path is only assumed
    // when the argument actually resolves to a directory on disk.
    std::error_code ec;
    if (query.find('\\') != std::string::npos
        || (query.find('/') != std::string::npos
            && fs::is_directory(fs::path(widen(query)), ec))) {
        const fs::path want = fs::absolute(fs::path(widen(query)), ec);
        for (const Project& p : v) {
            if (iequals(p.path.string(), want.string()))
                return &p;
        }
        *err = "no project at " + query;
        return nullptr;
    }

    // An exact hit on the qualified name settles it, which is how
    // "HHS Matrix/ocio-ato-modernization-site" picks one of three checkouts.
    for (const Project& p : v) {
        if (iequals(p.name, query) || iequals(p.displayName(), query))
            return &p;
    }

    std::vector<const Project*> hits;
    for (const Project& p : v) {
        if (istartsWith(p.name, query) || istartsWith(p.displayName(), query))
            hits.push_back(&p);
    }
    if (hits.empty()) {
        for (const Project& p : v) {
            if (icontains(p.displayName(), query))
                hits.push_back(&p);
        }
    }

    if (hits.size() == 1)
        return hits.front();

    if (hits.empty()) {
        *err = "no project matching \"" + query + "\"";
        return nullptr;
    }

    std::string list;
    for (size_t i = 0; i < hits.size() && i < 12; ++i)
        list += (i ? ", " : "") + hits[i]->displayName();
    if (hits.size() > 12)
        list += ", ...";
    *err = "\"" + query + "\" is ambiguous: " + list;
    return nullptr;
}

} // namespace

int main(int argc, char** argv)
{
    std::vector<std::string> raw;
    raw.reserve(static_cast<size_t>(argc));
    for (int i = 1; i < argc; ++i)
        raw.emplace_back(argv[i]);

    std::string parseErr;
    const Args  a = parseArgs(raw, &parseErr);

    if (!parseErr.empty())
        return failWith(kUsage, parseErr);
    if (!a.unknown.empty())
        return failWith(kUsage, "unknown option: " + a.unknown);

    if (a.version) {
        std::printf("ProjectMan %s\n", PM_VERSION_STRING);
        return kOk;
    }
    if (a.help) {
        printUsage();
        return kOk;
    }

    ConfigStatus status = ConfigStatus::Missing;
    std::string  detail;
    Config       cfg = Config::load(&status, &detail);

    if (status == ConfigStatus::Unreadable) {
        // The file is left exactly as it is. It is the only copy of something a
        // person typed, and a stray comma should not cost them their settings.
        std::fprintf(stderr, "projectman: %s\n", detail.c_str());
        std::fprintf(stderr, "projectman: using defaults; the file was not changed\n");
    } else if (status == ConfigStatus::Missing) {
        cfg.save(nullptr);
    }

    // Resolve "auto" once, here, so every later read and every printed value
    // is the number actually used.
    if (cfg.scanThreads <= 0)
        cfg.scanThreads = defaultScanThreads();

    if (!a.root.empty())
        cfg.root = fs::path(widen(a.root));
    if (!a.sort.empty())
        cfg.sort = a.sort;
    if (!cfg.gitExe.empty())
        git::setExePathOverride(cfg.gitExe);

    const std::string verb = a.verb.empty() ? std::string("browse") : a.verb;

    // ---------------------------------------------------------------- config
    if (verb == "config") {
        if (a.wantPath) {
            std::printf("%s\n", Config::filePath().string().c_str());
            return kOk;
        }
        if (a.wantInit) {
            std::string err;
            if (!cfg.save(&err))
                return fail(err);
            std::printf("wrote %s\n", Config::perUserPath().string().c_str());
            return kOk;
        }
        std::printf("config    %s\n", Config::filePath().string().c_str());
        std::printf("root      %s\n", cfg.root.string().c_str());
        std::printf("claude    %s\n", cfg.resolveClaude().string().c_str());
        std::printf("args      ");
        for (const std::string& s : cfg.claudeArgs)
            std::printf("%s ", s.c_str());
        std::printf("\nterminal  %s\n", cfg.resolveTerminal().string().c_str());
        std::printf("threads   %d\n", cfg.scanThreads);
        std::printf("dispatch  commit=%s push=%s max_repos=%d\n",
                    cfg.dispatchCommit ? "true" : "false",
                    cfg.dispatchPush ? "true" : "false", cfg.dispatchMaxRepos);
        return kOk;
    }

    // ---------------------------------------------------------------- doctor
    if (verb == "doctor") {
        const fs::path g  = git::exePath();
        const fs::path c  = cfg.resolveClaude();
        const fs::path wt = cfg.resolveTerminal();

        std::printf("%-10s %s\n", "git", g.empty() ? "NOT FOUND" : g.string().c_str());
        std::printf("%-10s %s\n", "claude", c.empty() ? "NOT FOUND" : c.string().c_str());
        std::printf("%-10s %s\n", "wt",
                    wt.empty() ? "not present (falls back to a new console)"
                               : wt.string().c_str());
        std::printf("%-10s %s\n", "config", Config::filePath().string().c_str());
        std::printf("%-10s %s\n", "root", cfg.root.string().c_str());

        if (g.empty())
            return failWith(kEnvironment, "git.exe not found; set git_exe in the config");

        NullSink    sink;
        ProjectList v = scanAll(cfg, sink);

        int repos = 0, containers = 0, bare = 0, folders = 0, dirty = 0;
        for (const Project& p : v) {
            switch (p.kind) {
            case ProjectKind::Repo:      ++repos; break;
            case ProjectKind::BareRepo:  ++bare; break;
            case ProjectKind::Container: ++containers; break;
            case ProjectKind::Plain:     ++folders; break;
            }
            if (p.git.state == GitState::Dirty)
                ++dirty;
        }

        std::printf("%-10s %zu entries: %d repos, %d containers, %d folders, %d bare\n",
                    "tree", v.size(), repos, containers, folders, bare);
        std::printf("%-10s %d probed, %d failed, %d dirty, %.2f s on %d threads\n",
                    "sweep", sink.probed_, sink.failed_, dirty, sink.seconds_,
                    cfg.scanThreads);
        return kOk;
    }

    if (git::exePath().empty())
        return failWith(kEnvironment, "git.exe not found; set git_exe in the config");

    // Every remaining verb needs the tree.
    NullSink    sink;
    ProjectList projects = scanAll(cfg, sink);
    sortProjects(projects, cfg.sort);

    // ---------------------------------------------------------------- browse
    if (verb == "browse") {
        cli::ConsoleSession con;

        // No console means output is redirected or this is CI. Printing the
        // list is the honest fallback; a TUI would emit escape codes into a
        // pipe.
        if (!con.acquire() || !con.enterTui()) {
            for (const Project& p : projects) {
                std::printf("%-42s %-26s %-12s %6s\n", p.displayName().substr(0, 42).c_str(),
                            p.git.branch.substr(0, 26).c_str(), stateWord(p.git).c_str(),
                            relativeTime(p.activityUnix()).c_str());
            }
            return kOk;
        }

        int exitCode = kOk;

        for (;;) {
            const cli::BrowseResult r = cli::browse(con, projects, cfg);
            if (r.action == cli::Action::Quit)
                break;

            // The console must be fully restored BEFORE the child is spawned:
            // it inherits this terminal, and it cannot be handed one still in
            // the alternate buffer with echo and line input switched off. The
            // output codepage stays UTF-8 for the child's lifetime.
            con.leaveTui(/*restoreCodepage=*/false);

            if (r.action == cli::Action::OpenTerminal) {
                std::string lerr;
                if (r.project && !openInTerminal([&] {
                        LaunchSpec s;
                        s.cwd = r.project->path;
                        return s;
                    }(), cfg, &lerr)) {
                    std::fprintf(stderr, "projectman: %s\n", lerr.c_str());
                }
                con.enterTui();
                continue;
            }

            LaunchSpec s;
            std::vector<Project*> touched;

            if (r.action == cli::Action::Dispatch) {
                s = dispatchSpec(r.plan, cfg);
                for (const fs::path& repo : r.plan.repos) {
                    for (Project& p : projects) {
                        if (p.path == repo)
                            touched.push_back(&p);
                    }
                }
            } else if (r.project) {
                s.cwd  = r.project->path;
                s.mode = r.action == cli::Action::LaunchContinue ? LaunchMode::Continue
                       : r.action == cli::Action::LaunchResume   ? LaunchMode::Resume
                                                                 : LaunchMode::New;
                for (Project& p : projects) {
                    if (p.path == r.project->path)
                        touched.push_back(&p);
                }
            }

            const HandoffResult h = handoff(s, cfg);
            if (!h.started) {
                std::fprintf(stderr, "projectman: %s\n", h.error.c_str());
                exitCode = kLaunchFail;
            } else {
                exitCode = h.exitCode;
            }

            if (cfg.onExit == OnChildExit::Quit) {
                con.leaveTui(true);
                return exitCode;
            }

            // Whatever it worked on has almost certainly changed. Re-probe only
            // those, which is a few tens of milliseconds rather than a sweep.
            git::ProbeOptions po;
            po.timeoutMs = cfg.probeTimeoutMs;
            for (Project* p : touched)
                probeProject(*p, po);

            sortProjects(projects, cfg.sort);
            con.enterTui();
        }

        con.leaveTui(true);
        return exitCode;
    }

    // -------------------------------------------------------------------- ls
    if (verb == "ls" || verb == "list") {
        int shown = 0;
        for (const Project& p : projects) {
            if (a.reposOnly && p.kind != ProjectKind::Repo)
                continue;
            if (a.dirty && p.git.state != GitState::Dirty)
                continue;

            if (a.json) {
                printProjectJson(p);
            } else {
                const std::string disp = p.displayName();
                std::printf("%-42s %-26s %-12s %-12s %6s  %s\n",
                            disp.substr(0, 42).c_str(),
                            p.git.branch.substr(0, 26).c_str(),
                            stateWord(p.git).c_str(), syncWord(p.git).c_str(),
                            relativeTime(p.activityUnix()).c_str(),
                            kindWord(p.kind).c_str());
            }
            ++shown;
        }
        if (!a.json && shown == 0)
            std::printf("nothing to show\n");
        return kOk;
    }

    // ---------------------------------------------------------------- status
    if (verb == "status") {
        if (a.positional.empty())
            return failWith(kUsage, "usage: pm status <name>");

        std::string  err;
        const Project* p = resolveOne(projects, a.positional[0], &err);
        if (!p)
            return failWith(kNoMatch, err);

        if (a.json) {
            printProjectJson(*p);
            return kOk;
        }

        std::printf("%s\n%s\n\n", p->displayName().c_str(), p->path.string().c_str());
        std::printf("  %-12s %s\n", "kind", kindWord(p->kind).c_str());
        if (!p->ownerRepo.empty())
            std::printf("  %-12s %s\n", "github", p->ownerRepo.c_str());
        if (p->git.state != GitState::NotARepo) {
            std::printf("  %-12s %s\n", "branch", p->git.branch.c_str());
            std::printf("  %-12s %s\n", "upstream",
                        p->git.hasUpstream ? p->git.upstream.c_str() : "none");
            std::printf("  %-12s %s\n", "sync", syncWord(p->git).c_str());
            std::printf("  %-12s %d staged, %d modified, %d untracked, %d conflicted\n",
                        "changes", p->git.staged, p->git.unstaged, p->git.untracked,
                        p->git.conflicted);
            if (p->git.stashes)
                std::printf("  %-12s %d\n", "stashes", p->git.stashes);
            if (p->git.lastCommitUnix) {
                std::printf("  %-12s %s ago by %s\n", "last commit",
                            relativeTime(p->git.lastCommitUnix).c_str(),
                            p->git.lastCommitAuthor.c_str());
                std::printf("  %-12s %s\n", "", p->git.lastCommitSubject.c_str());
            }
            if (!p->git.error.empty())
                std::printf("  %-12s %s\n", "error", p->git.error.c_str());
        }
        return kOk;
    }

    // ------------------------------------------------------------------ open
    if (verb == "open") {
        if (a.positional.empty())
            return failWith(kUsage, "usage: pm open <name>");

        std::string  err;
        const Project* p = resolveOne(projects, a.positional[0], &err);
        if (!p)
            return failWith(kNoMatch, err);

        LaunchSpec s;
        s.cwd   = p->path;
        s.mode  = a.cont ? LaunchMode::Continue
                         : (a.resume ? LaunchMode::Resume : LaunchMode::New);
        s.resumeId = a.resumeId;
        s.model    = a.model;

        std::string lerr;
        if (!openInTerminal(s, cfg, &lerr))
            return failWith(kLaunchFail, lerr);
        return kOk;
    }

    // -------------------------------------------------------------------- go
    if (verb == "go") {
        if (a.positional.empty())
            return failWith(kUsage, "usage: pm go <name>");

        std::string  err;
        const Project* p = resolveOne(projects, a.positional[0], &err);
        if (!p)
            return failWith(kNoMatch, err);

        LaunchSpec s;
        s.cwd      = p->path;
        s.mode     = a.cont ? LaunchMode::Continue
                            : (a.resume ? LaunchMode::Resume : LaunchMode::New);
        s.resumeId = a.resumeId;
        s.model    = a.model;

        const HandoffResult h = handoff(s, cfg);
        if (!h.started)
            return failWith(kLaunchFail, h.error);
        return h.exitCode;   // pm is a transparent wrapper
    }

    // ----------------------------------------------------------------- items
    if (verb == "items") {
        const WorkList items = collectWorkItems(projects);
        if (items.empty()) {
            if (!a.json)
                std::printf("nothing outstanding\n");
            return kOk;
        }
        for (const WorkItem& w : items) {
            if (a.json) {
                std::printf("{\"kind\":\"%s\",\"project\":\"%s\",\"path\":\"%s\","
                            "\"count\":%d,\"summary\":\"%s\"}\n",
                            kindLabel(w.kind), jsonEscape(w.project).c_str(),
                            jsonEscape(w.path.string()).c_str(), w.count,
                            jsonEscape(w.summary).c_str());
            } else {
                std::printf("%-13s %-28s %s\n", kindLabel(w.kind),
                            w.project.substr(0, 28).c_str(), w.summary.c_str());
            }
        }
        return kOk;
    }

    // -------------------------------------------------------------- dispatch
    if (verb == "dispatch") {
        WorkList items = collectWorkItems(projects);
        if (items.empty())
            return failWith(kNoMatch, "nothing outstanding to dispatch");

        if (a.all) {
            for (WorkItem& w : items)
                w.selected = true;
        } else {
            preselect(items, cfg.dispatchMaxRepos);
        }

        DispatchOptions opt;
        opt.allowCommit = cfg.dispatchCommit;
        opt.allowPush   = cfg.dispatchPush;
        opt.maxRepos    = cfg.dispatchMaxRepos;

        const DispatchPlan plan = buildDispatchPlan(items, opt);
        if (plan.items.empty())
            return failWith(kNoMatch, "nothing selected");

        if (a.dryRun) {
            std::printf("%s", plan.briefing.c_str());
            return kOk;
        }

        std::fprintf(stderr, "dispatching %zu items across %zu repositories\n",
                     plan.items.size(), plan.repos.size());

        const LaunchSpec s = dispatchSpec(plan, cfg);
        const HandoffResult h = handoff(s, cfg);
        if (!h.started)
            return failWith(kLaunchFail, h.error);
        return h.exitCode;
    }

    printUsage();
    return failWith(kUsage, "unknown command: " + verb);
}
