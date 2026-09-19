#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// The rest of what Claude Code leaves on disk.
//
// agent_board.h covers the three sources that make a card: the session
// registry, ProjectMan's own hook log, and a session's subagent and workflow
// files. This covers the four that make the node explorer show what a session
// is doing rather than only what it has spawned.
//
//   %TEMP%\claude\<slug>\<session>\tasks\<task>.output
//                          Every Bash, PowerShell and Monitor call the session
//                          has in flight. One file per call, named by the id
//                          Claude Code hands back ("b8jqv146x"), holding that
//                          call's output as it arrives.
//
//                          Not only shells: subagents write here too, under
//                          their own ids, and so do workflow agents. Nothing
//                          in a filename says which is which, which is the
//                          second reason the hook join decides what a task is
//                          rather than the directory listing. Nothing cleans
//                          the directory up either, so a session open since
//                          yesterday has every task it has ever run still in
//                          it, finished ones included.
//
//   ~/.claude/tasks/<session>/<n>.json
//                          The session's task list, one file per item, with
//                          real blocks/blockedBy edges between them. A graph,
//                          not a list, which is the whole reason it is worth
//                          drawing rather than printing.
//
//   ~/.claude/jobs/<short>/state.json, timeline.jsonl
//   ~/.claude/daemon/roster.json
//                          Claude Code sessions running headless under the
//                          background daemon. Real agents with a pid, a cwd and
//                          a rolling activity line, and invisible to everything
//                          else here: they never write a session registry file,
//                          so the board has never shown one.
//
//   ~/.claude/plans/<slug>.md
//                          Plan documents. The only one of the four that
//                          carries no session id of its own; see readPlans.
//
// Every one of these is undocumented and none is stable. The rules are
// agent_board.h's rules: every field optional, every unknown value information
// rather than an error, every read shared with its writer, every size capped.
namespace pm::gui {

// ------------------------------------------------------------ shells and monitors

// Which tool opened a task. Decided by the hook event that named the task id,
// so a session running without ProjectMan's hooks installed reports Unknown for
// all of them rather than guessing from the output.
enum class TaskKind { Unknown, Shell, Monitor };

const char* taskKindLabel(TaskKind k);

// One entry in a session's tasks directory that might still be going.
//
// Whether it IS still going is two different questions, and the split is the
// whole reason this struct has two fields for it rather than one bool.
//
// A shell holds its output file open for exactly as long as it runs, so an
// exclusive open answers it exactly: `writing` is that answer. Measured at 60
// samples out of 60 on a live background shell and 0 out of 60 on a finished
// one.
//
// A monitor never holds the file at all. Measured the same way on two monitors
// that were emitting events throughout: 0 samples out of 60. Nothing else it
// writes changes when it ends, so the file it leaves behind is indistinguishable
// from one still armed, and the only thing on the machine that says when it
// stops is the timeout it declared when it started. `expiresAtMs` is that,
// carried through the hook log; see src/cli/hook.h.
//
// Hence `live()` rather than a field. A caller must not test either half
// directly: a shell has no deadline and a monitor never holds a handle, so each
// looks permanently dead through the other one's test.
struct BackgroundTask {
    std::string id;      // "b8jqv146x", Claude Code's own id for the call
    TaskKind    kind = TaskKind::Unknown;

    // What the call was: the command for a shell, the description for a
    // monitor. Filled by the join in buildBoard, empty without hooks.
    std::string label;

    // The last non-empty line of output, which is what the call is saying right
    // now. Cut to kTailChars; the file itself is never held in full.
    std::string tail;

    // Somebody has the output file open for writing. Exact for a shell, always
    // false for a monitor.
    bool writing = false;

    // When a monitor's watch runs out, as Unix milliseconds. Zero for anything
    // that did not declare one, which is everything but a monitor and any
    // monitor whose PreToolUse has rolled off the back of the log.
    std::int64_t expiresAtMs = 0;

    std::int64_t lastOutputMs = 0;   // the file's last write

    // `nowMs` is passed in rather than read here so one refresh judges every
    // task against one clock.
    //
    // The monitor answer is an upper bound, not an observation: a watch whose
    // source ends early goes on being drawn until its declared timeout runs
    // out. That is bounded by Monitor's own cap of an hour and usually by its
    // five-minute default, and it is the honest limit of what disk can say.
    bool live(std::int64_t nowMs) const
    {
        if (kind == TaskKind::Monitor)
            return expiresAtMs > 0 && nowMs < expiresAtMs;
        return writing;
    }
};

// Finds a session's task directory, and remembers where it was.
//
// The directory sits under a slug of the session's working directory, and the
// encoding is Claude Code's own: every character outside [A-Za-z0-9] becomes a
// dash, so C:\Users\me\projects\App is C--Users-me-projects-App. That is
// derived rather than searched, because the alternative is listing a temp root
// that holds one directory per project this machine has ever opened, per
// refresh, per session.
//
// The derivation is a guess about an undocumented encoding, so a miss falls
// back to the scan it was avoiding, and the answer is kept either way. A
// session with nothing in flight has no directory at all, which is not an error
// and is not worth re-deriving every five seconds.
//
// Single-threaded, like SessionFileReader, and owned by the same controller.
class TaskReader {
public:
    // Most recently written first. `cwd` is what the slug is derived from;
    // empty is allowed and costs the scan. `nowMs` is the refresh's own clock,
    // passed in so every session is judged against one instant.
    //
    // Returns what MIGHT still be going, not what is: the two tests that would
    // settle it need the kind, and the kind comes from the hook log, which this
    // does not read. Ask BackgroundTask::live.
    std::vector<BackgroundTask> read(const std::string& sessionId, const fs::path& cwd,
                                     std::int64_t nowMs);

private:
    // An empty path is cached too: it means "looked, found nothing", which is
    // the common case and the one worth not repeating.
    const fs::path& directoryFor(const std::string& sessionId, const fs::path& cwd);

    std::unordered_map<std::string, fs::path> dirs_;
};

// ------------------------------------------------------------------- task list

// One item from a session's task list. Ids are the file's own stem ("1", "2"),
// which is what blocks and blockedBy name.
struct TodoItem {
    std::string id;
    std::string subject;      // "Core conversion library"
    std::string activeForm;   // "Building the core conversion library"
    std::string status;       // "pending", "in_progress", "completed"

    std::vector<std::string> blocks;
    std::vector<std::string> blockedBy;

    bool done() const { return status == "completed"; }
    bool active() const { return status == "in_progress"; }
};

// A session's task list, in id order. Empty when the session has none, which is
// most of them.
std::vector<TodoItem> readTodos(const std::string& sessionId);

// ------------------------------------------------------ background Claude sessions

// One Claude Code session running under the background daemon.
//
// These are agents in exactly the sense a foreground session is, and none of
// them appears in ~/.claude/sessions: the daemon keeps its own book. A job that
// has reached a terminal state keeps its directory for ever, so `live` is what
// separates the two, and it is answered by the daemon roster and the pid rather
// than by the state file, which is only as current as the last thing written to
// it.
struct BackgroundJob {
    std::string shortId;     // "0b01d8d4", what the directory is named
    std::string sessionId;
    std::string name;        // "Connecting Colorado profile"
    std::string intent;      // the prompt it was started on
    std::string state;       // "working", "done", whatever else it writes
    std::string detail;      // its rolling one-line activity
    fs::path    cwd;

    unsigned long pid    = 0;
    std::int64_t  tokens = 0;

    std::int64_t createdAtMs = 0;
    std::int64_t updatedAtMs = 0;

    bool live = false;

    bool working() const { return state == "working"; }
};

// Every job directory, live ones first, then most recently updated. Reads the
// daemon roster once and joins by short id.
std::vector<BackgroundJob> readJobs();

// -------------------------------------------------------------------- plans

// One plan document.
//
// The only source here with no session id in it. A plan file is named after the
// prompt that produced it and carries nothing else, so `sessionId` is filled by
// the join in buildBoard: ExitPlanMode fires a hook, and a plan file written
// within a few seconds of that event in that session is that session's plan.
// A plan whose session is not live, or that predates the event log's window,
// stays unattached and is not drawn.
struct PlanDoc {
    fs::path     file;
    std::string  title;       // the leading "# " heading, else the file stem
    std::string  sessionId;   // filled by the join, empty until then
    std::int64_t writtenAtMs = 0;
};

// Plans written no longer than `withinMs` ago. The directory keeps every plan
// ever made, and a graph of work in flight has no room for one from three weeks
// back. Newest first.
std::vector<PlanDoc> readPlans(std::int64_t withinMs);

// How far back readPlans looks by default.
//
// Wide on purpose, because this is a prefilter and not the answer. What decides
// whether a plan is shown is the join: it has to land on an ExitPlanMode event
// still in the hook log, in a session still live. Both of those age out on
// their own, and the log's own window is about a week.
//
// A day was the first value here and it was wrong in the case that matters. A
// session open since yesterday is still working on the plan it wrote when it
// started, and cutting at 24 hours took the plan off exactly the session most
// likely to still be following it.
inline constexpr std::int64_t kPlanWindowMs = 7 * 24 * 60 * 60 * 1000;

// ----------------------------------------------------------------- locations

// %TEMP%\claude. Empty when the temp path cannot be resolved. This is where
// Claude Code puts a session's scratchpad and its task output, and it honours
// TMP and TEMP the same way GetTempPathW does.
fs::path claudeTempRoot();

// ~/.claude/tasks, ~/.claude/jobs, ~/.claude/plans. Empty when the profile
// cannot be resolved.
fs::path todoRoot();
fs::path jobRoot();
fs::path planRoot();

} // namespace pm::gui
