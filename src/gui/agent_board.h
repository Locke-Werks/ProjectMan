#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

// Every Claude Code session on this machine, and what it is doing.
//
// Three sources, and the split between them is the whole design:
//
//   ~/.claude/sessions/<pid>.json   Claude Code's own live registry, one file
//                                   per running process. Owns the COLUMN: it is
//                                   the only thing that knows a session is
//                                   alive, and the only thing still true after
//                                   a session is killed without warning.
//
//   %LOCALAPPDATA%\ProjectMan\agents\events.jsonl
//                                   What `pm hook` appends. Owns the CARD FACE:
//                                   the prompt, the tool call in flight, the
//                                   question being asked. Hooks cannot report a
//                                   session that died, so they never decide
//                                   whether one is alive.
//
//   ~/.claude/projects/<slug>/<sessionId>/
//                                   Claude Code's per-session files. Owns
//                                   IDENTITY for the work inside a session: a
//                                   subagent's type and description, and which
//                                   workflow run and phase it belongs to. None
//                                   of it says whether anything is still
//                                   running, so it never decides state.
//
// The one crossing: an unanswered Notification moves a card to NeedsYou whatever
// the registry says, because a session waiting on a permission prompt still
// reports itself busy.
//
// None of the three is documented, and the registry carries the CLI version that
// wrote it. Treat every field as optional and every unknown value as information
// rather than an error, the way claude_stream.cpp treats an unrecognised event.
namespace pm::gui {

enum class AgentColumn {
    NeedsYou,   // asked a question, or wants permission
    Working,    // a turn in flight
    Idle,       // alive, turn handed back
    Done,       // the process is gone
};

const char* columnLabel(AgentColumn c);

// Every column, in display order. NeedsYou first: the board is read left to
// right and the leftmost column is the one worth crossing the room for.
const std::vector<AgentColumn>& columnOrder();

// ---------------------------------------------------------------- the sources

// One ~/.claude/sessions/<pid>.json, as far as it can be trusted.
//
// Field names are Claude Code's, not ours. Observed on 2.1.270: pid, sessionId,
// cwd, startedAt, procStart, version, peerProtocol, peerFeatures, kind,
// entrypoint, pidDomain, messagingSocketPath, name, nameSource, nameSince,
// status, updatedAt, statusUpdatedAt, bridgeSessionId.
struct RegistryEntry {
    unsigned long pid = 0;
    std::string   sessionId;
    fs::path      cwd;
    std::string   name;          // "projectman-08", derived by Claude Code
    std::string   status;        // "busy" seen; treat anything else as not busy
    std::string   waitingFor;    // optional; any non-empty value means NeedsYou
    std::string   kind;          // "interactive"
    std::string   entrypoint;    // "cli"
    std::string   version;       // the CLI that wrote this record
    std::int64_t  startedAtMs       = 0;
    std::int64_t  updatedAtMs       = 0;
    std::int64_t  statusUpdatedAtMs = 0;

    // Observed on 2.1.270: "busy" while a turn is in flight, and "waiting" when
    // the session wants something, carrying waitingFor "input needed". Both
    // were read off live sessions rather than found in the CLI, so the set is
    // not known to be closed and neither test is an else for the other.
    bool busy() const { return status == "busy"; }
    bool waiting() const { return status == "waiting"; }
};

// One line of events.jsonl, as written by `pm hook`. The shape is fixed in
// src/cli/hook.h and the two must agree.
struct AgentEvent {
    std::int64_t tsMs = 0;
    std::string  event;     // hook_event_name: SessionStart, PreToolUse, ...
    std::string  sessionId;
    fs::path     cwd;
    std::string  tool;      // PreToolUse only
    std::string  detail;    // see hook.h for what this carries per event

    // A subagent keeps its parent's session_id, so without these two an event
    // from inside one is indistinguishable from the parent's own. Empty means
    // the session itself acted. Verified on CLI 2.1.276: every event fired
    // inside a subagent carries both, including its tool calls.
    std::string agentId;
    std::string agentType;

    // PreToolUse only. On the parent's own Agent call this is what the spawned
    // subagent's meta file carries as toolUseId, which is what ties a running
    // subagent back to the description it was given.
    std::string toolUseId;
};

// What is known about one subagent.
//
// Deliberately not a bool. A meta file proves a subagent started and says
// nothing about whether it ended, so "we saw it start" and "it is running right
// now" are different claims and only hooks can make the second one.
enum class AgentState {
    Spawned,    // a meta file exists; no hook has spoken for it
    Running,    // SubagentStart, and no SubagentStop yet
    Finished,   // SubagentStop, or its workflow run ended
};

struct SubAgent {
    std::string id;            // agent_id, which is also its meta filename
    std::string type;          // "Explore", "code-reviewer", "workflow-subagent"
    std::string description;   // what it was asked to do
    std::string workflowRun;   // wf_<id>, empty outside a workflow
    std::string phase;         // the run's phase it belongs to, empty otherwise
    std::string activity;      // its own tool line, from hooks

    AgentState   state          = AgentState::Spawned;
    std::int64_t startedAtMs    = 0;
    std::int64_t lastActivityMs = 0;
};

// One Workflow tool run, folded from the two files it writes.
struct WorkflowRun {
    std::string runId;    // wf_a3f3cbaa-9df
    std::string name;     // the script's own meta.name
    std::string phase;    // the phase its most recent agent was spawned into
    std::string status;   // "running" until the summary lands, then its own

    int          spawned     = 0;
    int          running     = 0;   // 0 when no hooks are installed
    std::int64_t startedAtMs = 0;

    bool finished() const { return status != "running"; }
};

// Read every registry file in `sessionsDir`, skipping any whose process is
// gone. The registry keeps files for processes that have exited, so a pid that
// no longer exists is a stale file rather than a live session, and a pid alone
// is not proof: Windows reuses them, so the record's start time is checked too.
std::vector<RegistryEntry> readRegistry(const fs::path& sessionsDir);

// Parse events.jsonl. Malformed lines are skipped rather than failing the read:
// a half-written line from a session that was killed mid-append must not cost
// the board every event behind it.
std::vector<AgentEvent> readEvents(const fs::path& eventLog, std::int64_t sinceMs = 0);

// One session's own directory, read for what happened inside it.
struct SessionFiles {
    std::string               sessionId;
    std::vector<SubAgent>     agents;
    std::vector<WorkflowRun>  workflows;
};

// Reads those directories, and remembers what it has already read.
//
// Both caches exist because the alternative is quadratic in a session's own
// history. A session's directory never moves, so finding it is done once; a
// meta file is written when its subagent spawns and never rewritten, so a
// subagent that ran an hour ago is parsed once and never opened again. Without
// that, every refresh would scan ~/.claude/projects once per live session and
// reopen every meta file that session ever wrote, and a machine that uses
// subagents at all accumulates hundreds of them with nothing ever cleaning
// them up.
//
// A finished workflow is cached the same way and for the same reason: its
// summary carries the whole script it ran, which is tens of kilobytes, and it
// cannot change again once it has a terminal status.
//
// Single-threaded. BoardController owns one and only its worker touches it.
class SessionFileReader {
public:
    // `cwd` narrows the search for the session's directory to the one slug that
    // can hold it. Empty is allowed and costs a full scan.
    SessionFiles read(const std::string& sessionId, const fs::path& cwd);

private:
    const fs::path& directoryFor(const std::string& sessionId, const fs::path& cwd);

    std::unordered_map<std::string, fs::path>    dirs_;
    std::unordered_map<std::string, SubAgent>    metas_;      // by agent id
    std::unordered_map<std::string, WorkflowRun> finished_;   // by run id
};

// ~/.claude/projects. Empty when the profile cannot be resolved.
fs::path sessionDataRoot();

// ------------------------------------------------------------------ the board

struct AgentCard {
    std::string sessionId;
    std::string name;        // from the registry; falls back to the cwd leaf
    fs::path    cwd;
    std::string project;     // ProjectMan's display name for cwd, when it knows one
    AgentColumn column = AgentColumn::Done;

    std::string activity;    // "Edit src/gui/agent_board.cpp", from PreToolUse
    std::string prompt;      // the last thing asked of it, from UserPromptSubmit
    std::string attention;   // what it is waiting for, when column is NeedsYou

    // What the session has running underneath it. A subagent is not a card of
    // its own: it has no process, no window and no prompt to answer, so there
    // is nothing on it FOCUS, ENGAGE or DISPATCH could act on. It belongs to
    // the session that can be acted on.
    //
    // Running first, then most recently active. Workflow runs newest first.
    std::vector<SubAgent>    agents;
    std::vector<WorkflowRun> workflows;

    unsigned long pid = 0;
    std::int64_t  startedAtMs    = 0;
    std::int64_t  lastActivityMs = 0;
    bool          live           = false;   // a registry entry exists

    // Subagents not accounted for by any of `workflows`, which is what the card
    // lists on its own.
    int looseAgents(AgentState state) const;
};

using AgentList = std::vector<AgentCard>;

// Fold the two sources into one board.
//
// `projects` is ProjectMan's scanned tree, used only to give a card the same
// project name the table shows. A session in a directory ProjectMan does not
// index still gets a card, named after its own folder: an agent working
// somewhere unindexed is exactly the one worth seeing.
//
// `files` is one entry per session whose directory was read, joined by session
// id. A session missing from it simply has no subagents to show.
//
// Ordering within a column is most recently active first.
AgentList buildBoard(const std::vector<RegistryEntry>& registry,
                     const std::vector<AgentEvent>&    events,
                     const std::vector<SessionFiles>&  files,
                     const ProjectList&                projects);

// ------------------------------------------------------------------ locations

// %LOCALAPPDATA%\ProjectMan\agents\events.jsonl. Empty when LocalAppData cannot
// be resolved. Must agree with src/cli/hook.cpp, which writes it.
fs::path eventLogPath();

// %USERPROFILE%\.claude\sessions. Empty when the profile cannot be resolved.
fs::path sessionRegistryDir();

} // namespace pm::gui
