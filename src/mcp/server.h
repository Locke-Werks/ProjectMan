#pragma once

#include "json.h"

#include <string>

namespace pm::mcp {

// The tool this server exists for. One name, in one place, because it appears
// in the listing, in the dispatch, and in the refusal when it is misspelled.
inline constexpr char kToolName[] = "get-projects";

inline constexpr char kServerName[] = "projectman";

// Handles one incoming JSON-RPC message.
//
// Returns false when there is nothing to send back, which is the correct and
// required behaviour for a notification: answering one is a protocol violation,
// not a harmless extra.
bool handleMessage(const json::Value& request, json::Value* response);

// Parses, dispatches and serialises one line. Returns the line to write, or an
// empty string when nothing should be written.
//
// A line that is not JSON at all still gets a -32700 with a null id, because a
// client that sent garbage is owed an answer it can correlate to nothing rather
// than silence it will wait on forever.
std::string handleLine(std::string_view line);

// Reads newline-delimited JSON from stdin and writes it to stdout until stdin
// closes. Returns a process exit code.
int run();

} // namespace pm::mcp
