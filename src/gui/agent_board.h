#pragma once

#include "model.h"

#include <cstdint>
#include <string>
#include <vector>

// Every Claude Code session on this machine, and what it is doing.
//
// Two sources, and the split between them is the whole design:
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
// The one crossing: an unanswered Notification moves a card to NeedsYou whatever
// the registry says, because a session waiting on a permission prompt still
// reports itself busy.
//
// The registry is undocumented and carries the CLI version that wrote it. Treat
// every field as optional and every unknown value as information rather than an
// error, the way claude_stream.cpp treats an unrecognised event.
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

    unsigned long pid = 0;
    std::int64_t  startedAtMs    = 0;
    std::int64_t  lastActivityMs = 0;
    bool          live           = false;   // a registry entry exists
};

using AgentList = std::vector<AgentCard>;

// Fold the two sources into one board.
//
// `projects` is ProjectMan's scanned tree, used only to give a card the same
// project name the table shows. A session in a directory ProjectMan does not
// index still gets a card, named after its own folder: an agent working
// somewhere unindexed is exactly the one worth seeing.
//
// Ordering within a column is most recently active first.
AgentList buildBoard(const std::vector<RegistryEntry>& registry,
                     const std::vector<AgentEvent>&    events,
                     const ProjectList&                projects);

// ------------------------------------------------------------------ locations

// %LOCALAPPDATA%\ProjectMan\agents\events.jsonl. Empty when LocalAppData cannot
// be resolved. Must agree with src/cli/hook.cpp, which writes it.
fs::path eventLogPath();

// %USERPROFILE%\.claude\sessions. Empty when the profile cannot be resolved.
fs::path sessionRegistryDir();

} // namespace pm::gui
