#include "projects.h"

#include "discovery.h"
#include "enrich.h"
#include "git.h"
#include "scanner.h"

#include <chrono>

namespace pm::mcp {
namespace {

const char* kindName(ProjectKind k)
{
    switch (k) {
    case ProjectKind::Repo:      return "repo";
    case ProjectKind::BareRepo:  return "bare";
    case ProjectKind::Container: return "container";
    case ProjectKind::Plain:     return "folder";
    }
    return "unknown";
}

const char* stateName(GitState s)
{
    switch (s) {
    case GitState::Unknown:  return "unknown";
    case GitState::NotARepo: return "not-a-repo";
    case GitState::Bare:     return "bare";
    case GitState::Error:    return "error";
    case GitState::Clean:    return "clean";
    case GitState::Dirty:    return "dirty";
    }
    return "unknown";
}

json::Value str(const std::string& s) { return json::makeString(s); }
json::Value num(int n) { return json::makeInt(n); }
json::Value num(std::int64_t n) { return json::makeInt(n); }

// The scan calls a sink from worker threads. Nothing here needs progressive
// delivery, so this only has to be safe to call concurrently and do nothing.
class Silent : public StatusSink {
public:
    void onStatus(std::size_t, const Project&) override {}
    void onScanFinished(int probed, int failed, double wallSeconds) override
    {
        probed_ = probed;
        failed_ = failed;
        wall_   = wallSeconds;
    }

    int    probed() const { return probed_; }
    int    failed() const { return failed_; }
    double wall() const { return wall_; }

private:
    int    probed_ = 0;
    int    failed_ = 0;
    double wall_   = 0;
};

} // namespace

json::Value projectToJson(const Project& p)
{
    const GitStatus& g = p.git;
    const OpenItems& o = p.open;

    std::vector<std::pair<std::string, json::Value>> git = {
        { "state", str(stateName(g.state)) },
        { "branch", str(g.branch) },
        { "headOid", str(g.headOid) },
        { "hasUpstream", json::makeBool(g.hasUpstream) },
        { "upstream", str(g.upstream) },
        { "ahead", num(g.ahead) },
        { "behind", num(g.behind) },
        { "staged", num(g.staged) },
        { "unstaged", num(g.unstaged) },
        { "untracked", num(g.untracked) },
        { "conflicted", num(g.conflicted) },
        { "stashes", num(g.stashes) },
        { "dirtyCount", num(g.dirtyCount()) },
        { "lastCommitUnix", num(g.lastCommitUnix) },
        { "lastCommitSubject", str(g.lastCommitSubject) },
        { "lastCommitAuthor", str(g.lastCommitAuthor) },
    };
    if (!g.error.empty())
        git.emplace_back("error", str(g.error));

    std::vector<std::pair<std::string, json::Value>> open = {
        { "total", num(o.total()) },
        { "uncommitted", num(o.uncommitted) },
        { "unpushed", num(o.unpushed) },
        { "unpulled", num(o.unpulled) },
        { "stashes", num(o.stashes) },
        { "openPullRequests", num(o.openPrs) },
        { "openIssues", num(o.openIssues) },
        { "uncheckedChecklistItems", num(o.checklistItems) },
        { "claudeSessions", num(o.claudeSessions) },
        { "claudePrompts", num(o.claudePrompts) },
        { "claudeLastUnix", num(o.claudeLastUnix) },
        { "claudeTitle", str(o.claudeTitle) },
        { "claudeLastPrompt", str(o.claudeLastPrompt) },
    };

    return json::makeObject({
        { "name", str(p.name) },
        { "displayName", str(p.displayName()) },
        { "path", str(p.path.string()) },
        { "container", str(p.container.empty() ? std::string() : p.container.string()) },
        { "kind", str(kindName(p.kind)) },
        { "isRepo", json::makeBool(p.isRepo()) },
        { "hasClaudeMd", json::makeBool(p.hasClaudeMd) },
        { "originUrl", str(p.originUrl) },
        { "ownerRepo", str(p.ownerRepo) },
        { "activityUnix", num(p.activityUnix()) },
        { "git", json::makeObject(std::move(git)) },
        { "open", json::makeObject(std::move(open)) },
    });
}

ScanOutcome collectProjects()
{
    ScanOutcome out;

    // Loaded, never created. ConfigStatus::Missing is the whole reason this
    // product requires ProjectMan: without its config there is no projects root
    // to report on, and guessing one would be worse than saying so.
    ConfigStatus status = ConfigStatus::Loaded;
    std::string  detail;
    const Config cfg = Config::load(&status, &detail);

    if (status == ConfigStatus::Missing) {
        out.error = "ProjectMan is not set up on this machine: no config at "
                    + Config::filePath().string()
                    + ". Run ProjectMan or `pm` once to create it.";
        return out;
    }
    if (status == ConfigStatus::Unreadable) {
        out.error = "ProjectMan's config could not be read. " + detail;
        return out;
    }

    std::error_code ec;
    if (cfg.root.empty() || !fs::is_directory(cfg.root, ec)) {
        out.error = "The configured projects root does not exist: " + cfg.root.string();
        return out;
    }

    DiscoveryOptions opts;
    opts.root              = cfg.root;
    opts.descendContainers = cfg.descendContainers;
    opts.includePlain      = cfg.includePlain;
    opts.exclude           = cfg.exclude;

    ProjectList projects = discover(opts, nullptr);

    git::ProbeOptions po;
    po.timeoutMs = cfg.probeTimeoutMs;

    Silent      sink;
    CancelToken token;

    const auto began = std::chrono::steady_clock::now();

    ScanPool pool(cfg.scanThreads, po);
    pool.start(projects, sink, token);
    pool.wait();

    // The same three passes both front ends run, in the same order. Skipping
    // them would report a project as having nothing outstanding when it has
    // four open pull requests.
    enrichClaudeState(projects, cfg);
    enrichChecklists(projects, cfg);
    enrichGitHub(projects, cfg, /*force=*/false, nullptr, nullptr);

    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();

    std::vector<json::Value> rows;
    rows.reserve(projects.size());

    int repos = 0, dirty = 0, openTotal = 0;
    for (const Project& p : projects) {
        if (p.isRepo())
            ++repos;
        if (p.git.state == GitState::Dirty)
            ++dirty;
        openTotal += p.open.total();
        rows.push_back(projectToJson(p));
    }

    out.payload = json::makeObject({
        { "root", str(cfg.root.string()) },
        { "scannedAtUnix",
          json::makeInt(std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count()) },
        { "scanSeconds", json::makeNumber(seconds) },
        { "counts", json::makeObject({
                        { "projects", num(static_cast<int>(projects.size())) },
                        { "repos", num(repos) },
                        { "dirty", num(dirty) },
                        { "openItems", num(openTotal) },
                        { "probeFailures", num(sink.failed()) },
                    }) },
        { "projects", json::makeArray(std::move(rows)) },
    });

    out.ok = true;
    return out;
}

} // namespace pm::mcp
