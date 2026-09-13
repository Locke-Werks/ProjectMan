#pragma once

#include "config.h"
#include "json.h"
#include "proc.h"
#include "workitems.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What `claude -p --output-format stream-json` says, turned into something two
// front ends can show the same way.
//
// The stream is one JSON object per line. Nothing here drops a line: what is
// not recognised comes through as Unknown or Malformed with the text intact, so
// a new event type or a stray diagnostic is shown rather than lost.
namespace pm::claude {

enum class EventKind {
    Init,        // system/init: the session id and model
    Text,        // an assistant text block
    Thinking,    // an assistant thinking block
    ToolUse,     // an assistant tool_use block
    ToolResult,  // a user tool_result block
    Result,      // the final result event
    RateLimit,   // rate_limit_event
    System,      // system with any other subtype
    Unknown,     // JSON of a shape this does not know
    Malformed,   // not JSON at all
    Stderr,      // a line from the child's stderr
};

struct Event {
    EventKind   kind = EventKind::Unknown;
    std::string raw;               // the line as received, always set

    std::string sessionId;         // when the line carried one
    std::string text;              // Text, Thinking, ToolResult, Result, Stderr, raw text
    std::string toolName;          // ToolUse
    std::string toolSummary;       // ToolUse: the command, the path, the pattern
    std::string model;             // Init
    std::string apiKeySource;      // Init: "none" when the session runs on a login
    std::string subtype;           // Result ("success", "error_max_turns"...), System

    bool   isError           = false;   // ToolResult, Result
    double costUsd           = 0;       // Result
    int    durationMs        = 0;       // Result
    int    numTurns          = 0;       // Result
    int    permissionDenials = 0;       // Result: tool calls refused at this autonomy
};

// One stdout line to zero or more events. An assistant line carries a content
// array, and each block is its own event.
std::vector<Event> parseLine(std::string_view line);

// The one thing about a tool call worth a line: the command, the file, the
// pattern. Falls back to the description Claude wrote, then to the first string
// it was given at all.
//
// Exposed because a hook payload carries the same tool_name and tool_input as
// the stream does, and a tool call should read the same wherever it is shown.
std::string toolSummary(const std::string& name, const json::Value* input);

Event stderrEvent(std::string line);

enum class LineStyle { Text, Tool, Error, Raw, Meta };

struct RenderedLine {
    std::string text;
    LineStyle   style = LineStyle::Text;
};

// What a person should see for an event, or nothing: thinking, rate limits,
// the init event, results that succeeded and the final result all print
// nothing, the last because its text repeats the assistant's final message.
std::optional<RenderedLine> render(const Event& e);

// Fed every event, by both front ends, so the summary at the end is drawn from
// the same facts on each.
struct RunStats {
    std::string sessionId;
    std::string model;
    std::string apiKeySource;
    int         toolCalls = 0;
    bool        sawResult = false;
    Event       result;
    std::string lastStderr;
};

void account(RunStats& s, const Event& e);

enum class RunState { Done, Stopped, Failed };

struct RunSummary {
    RunState    state = RunState::Failed;
    std::string headline;   // "DONE  13.2 s  2 turns  $0.04"
};

RunSummary summarize(const RunStats& s, const StreamResult& r);

// The header lines a run opens with: what is being dispatched, at which rung,
// then one line per repository.
std::vector<std::string> describeRun(const DispatchPlan& plan, const Config& cfg);

// "13.2 s" under a minute, "1m 32s" over it.
std::string describeDuration(int ms);

} // namespace pm::claude
