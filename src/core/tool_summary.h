#pragma once

#include <cstddef>
#include <string>

namespace pm {
namespace json {
struct Value;
}

namespace claude {

// The one thing about a tool call worth a line: the command, the file, the
// pattern. Falls back to the description Claude wrote, then to the first string
// it was given at all, which is what makes an MCP tool nobody has a case for
// still say something useful.
std::string toolSummary(const std::string& name, const json::Value* input);

// The whole of what a tool call was given, as one line: the command, the path,
// the pattern, the URL or the query, uncut, with line breaks and tabs turned
// into spaces. Anything without a case of its own, every MCP tool included, is
// its compact JSON input. Only `max` cuts it, a bound against a pathological
// input rather than a length anyone is meant to read to.
std::string toolText(const std::string& name, const json::Value* input, std::size_t max = 2000);

// First line only, cut to `max`, with " ..." marking what was dropped.
//
// The cut backs off to a UTF-8 lead byte. A prompt is typed by a person and can
// carry anything, and half a codepoint written into the log arrives on the
// canvas as a replacement character.
std::string oneLine(std::string s, std::size_t max);

// A string member, or empty for a missing key, a null object, or a member that
// is there and is not a string.
std::string stringField(const json::Value* object, const char* key);

// What both the hook and the canvas cut a line to, so a line is a line wherever
// it came from.
inline constexpr std::size_t kSummaryChars = 120;

} // namespace claude
} // namespace pm
