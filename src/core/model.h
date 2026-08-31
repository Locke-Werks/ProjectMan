#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace pm {

namespace fs = std::filesystem;

// What a row in the list actually is. Container is a directory that is not
// itself a repo but holds repos one level down: security-reviews holds seven,
// HHS General Ops holds four.
enum class ProjectKind {
    Repo,
    BareRepo,   // DeadLetter-backup-mirror.git; status is invalid against it
    Container,
    Plain,
};

enum class GitState {
    Unknown,    // not probed yet; the list shows it pending
    NotARepo,
    Bare,
    Error,
    Clean,
    Dirty,
};

struct GitStatus {
    GitState state = GitState::Unknown;

    std::string branch;    // "# branch.head", or "(detached)"
    std::string headOid;   // "# branch.oid", or "(initial)" on an unborn branch

    // Git omits both "# branch.upstream" and "# branch.ab" when a branch has no
    // upstream, rather than reporting zero. Absent and "+0 -0" mean different
    // things, so the flag is tracked rather than inferred from the counters.
    std::string upstream;
    bool        hasUpstream = false;
    int         ahead  = 0;
    int         behind = 0;

    int staged     = 0;
    int unstaged   = 0;
    int untracked  = 0;
    int conflicted = 0;
    int stashes    = 0;    // "# stash <N>", from --show-stash

    std::int64_t lastCommitUnix = 0;
    std::string  lastCommitSubject;
    std::string  lastCommitAuthor;

    std::string error;      // stderr when state == Error
    int         elapsedMs = 0;

    int  dirtyCount() const { return staged + unstaged + untracked + conflicted; }
    bool dirty() const      { return dirtyCount() > 0; }
};

// Everything both front ends call an "open item".
struct OpenItems {
    int uncommitted = 0;   // staged + unstaged + untracked + conflicted
    int unpushed    = 0;   // ahead
    int unpulled    = 0;   // behind
    int stashes     = 0;

    int openPrs        = 0;
    int openIssues     = 0;
    int checklistItems = 0;

    // From ~/.claude. history.jsonl carries an unmangled absolute project path,
    // so these come from a forward lookup and never from inverting a slug.
    std::string  claudeTitle;       // newest transcript's "ai-title"
    std::string  claudeLastPrompt;  // newest transcript's "last-prompt"
    std::int64_t claudeLastUnix = 0;
    int          claudeSessions = 0;
    int          claudePrompts  = 0;

    int total() const
    {
        return uncommitted + unpushed + stashes + openPrs + openIssues + checklistItems;
    }
};

struct Project {
    std::string name;        // the directory's own name, display form
    fs::path    path;        // absolute
    fs::path    container;   // empty at top level; the parent for nested repos

    ProjectKind kind = ProjectKind::Plain;
    bool        hasClaudeMd = false;

    std::string originUrl;
    // "lockewerks/DeadLetter". Resolved from the origin URL, never from the
    // folder name: AAP is apogee-asset-pipeline, MTGSim is tolaria, MilkDrop3
    // is milkrun, and lockewerks-universal-installer is Forge.
    std::string ownerRepo;

    GitStatus git;
    OpenItems open;

    // Deliberately no size field. `du -sm` over this tree measured 8 minutes.

    bool isRepo() const { return kind == ProjectKind::Repo; }

    // Qualified by the container it lives in, so the three separate checkouts
    // of ocio-ato-modernization-site are distinguishable in a list. They sit at
    // different paths on different branches, so collapsing them to one row
    // would hide two real working trees.
    std::string displayName() const
    {
        if (container.empty())
            return name;
        return container.filename().string() + "/" + name;
    }

    // The default sort key. Prefers real commit time, falling back to Claude
    // Code activity for folders that have no git history at all.
    std::int64_t activityUnix() const
    {
        return git.lastCommitUnix > open.claudeLastUnix ? git.lastCommitUnix
                                                        : open.claudeLastUnix;
    }
};

using ProjectList = std::vector<Project>;

// Cooperative cancellation, shared by the scanner and both front ends.
class CancelToken {
public:
    void request_stop() noexcept { stop_.store(true, std::memory_order_relaxed); }
    bool stop_requested() const noexcept { return stop_.load(std::memory_order_relaxed); }
    void reset() noexcept { stop_.store(false, std::memory_order_relaxed); }

private:
    std::atomic<bool> stop_{ false };
};

// How scan results reach a front end.
//
// This exists instead of QFuture/QFutureWatcher because progressive delivery
// through a QFutureWatcher is a QObject signal, which needs a running event
// loop. pm.exe has none: its loop blocks on a console handle. A sink lets the
// same scanner feed the TUI through a wake event and the GUI through a queued
// connection, with no Qt in core at all.
class StatusSink {
public:
    virtual ~StatusSink() = default;

    // Called FROM A WORKER THREAD. Must not block and must not touch any UI.
    virtual void onStatus(std::size_t index, const Project& project) = 0;
    virtual void onScanFinished(int probed, int failed, double wallSeconds) = 0;
};

} // namespace pm
