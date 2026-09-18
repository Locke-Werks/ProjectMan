#pragma once

#include "model.h"

#include <string>
#include <vector>

// `pm hook`: Claude Code's hook receiver, and the commands that register it.
//
// With no argument this is the receiver. Claude Code runs it for every hook
// event in every session, feeding the payload on stdin. It reads that payload,
// appends one line to the event log, and exits. ProjectMan's board tails the
// log; nothing else reads it.
//
// Being on the critical path of every tool call sets the rules:
//
//   - It must not load the config. Config::load writes a default file when none
//     exists, and a hook is the wrong thing to be creating settings behind
//     someone's back. It must work with no projectman.toml at all.
//   - No git, no scan, no network. Open, append, close.
//   - It always exits 0. A hook's non-zero exit is visible inside the session
//     that ran it, and no board is worth putting an error in front of someone
//     mid-task. Failures are dropped silently: the registry still carries the
//     session, so a lost event costs detail, never the card.
//
// The management subcommands are not on that path and report failure normally.
namespace pm::cli {

// The record appended to events.jsonl, one compact JSON object per line:
//
//   {"ts":1789311216420,"event":"PreToolUse","session":"<uuid>",
//    "cwd":"C:\\Users\\you\\projects\\ProjectMan",
//    "tool":"Edit","detail":"src/gui/agent_board.cpp"}
//
//   ts       int, milliseconds since the Unix epoch
//   event    the hook_event_name, verbatim
//   session  session_id
//   cwd      the session's working directory
//   tool     tool_name, on PreToolUse only; absent otherwise
//   detail   what the card should show, by event:
//              SessionStart       source ("startup", "resume", "compact")
//              UserPromptSubmit   the prompt, first line, cut to fit
//              PreToolUse         claude::toolSummary of tool_input
//              Notification       message: what it is asking for
//              Stop               empty
//              SubagentStart      empty; agentType carries it
//              SubagentStop       empty
//              SessionEnd         reason
//
//   agent      agent_id, present on anything a SUBAGENT did
//   agentType  agent_type: "Explore", "workflow-subagent", a named agent
//   toolUse    tool_use_id, PreToolUse only
//
// Those last three are why the board can tell a session's own work from its
// subagents'. A subagent runs under its parent's session_id and reports its
// tool calls through the same hooks, so without agent_id a card shows whichever
// of five subagents called a tool last as what the session itself is doing.
// Verified on CLI 2.1.276, where the parent's own spawn call is tool_name
// "Agent" and carries no agent_id.
//
// Keys are omitted when empty rather than written as "". src/gui/agent_board.h
// declares the reading half and the two must agree.

// The verb, dispatched from main.cpp before the config is loaded.
// `args` is everything after "hook".
int hookCommand(const std::vector<std::string>& args);

// Set this to a directory and both paths below move inside it:
//
//   <dir>\settings.json            instead of %USERPROFILE%\.claude\settings.json
//   <dir>\agents\events.jsonl      instead of %LOCALAPPDATA%\ProjectMan\agents\...
//
// It exists because there is otherwise no way to exercise install, uninstall or
// the receiver without writing to the settings file Claude Code is reading, and
// a registration bug that eats that file is the one failure here that costs
// somebody real work. A test points this at a scratch directory holding a copy.
//
// Honoured by the receiver and by the management subcommands alike, so an
// install and the events it later produces land in the same scratch tree. Unset
// or empty means the real locations, which is every ordinary run.
constexpr const wchar_t* kTestRootVar = L"PM_HOOK_ROOT";

// %LOCALAPPDATA%\ProjectMan\agents\events.jsonl, beside projectman.toml and
// github.tsv. Empty when LocalAppData cannot be resolved.
fs::path eventLogPath();

// Hooks fire per tool call across every session at once, so the log is capped.
// Past kMaxLogBytes it is rolled to events.1.jsonl and started again, keeping
// one back-file. The board reads both.
constexpr std::int64_t kMaxLogBytes = 4 * 1024 * 1024;

} // namespace pm::cli
