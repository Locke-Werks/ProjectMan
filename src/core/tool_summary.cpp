#include "tool_summary.h"

#include "json.h"

namespace pm::claude {

std::string stringField(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::String) ? v->string : std::string();
}

namespace {

// Back off to a lead byte. A continuation byte is 10xxxxxx, so walking back off
// those lands on the start of the codepoint that straddled the cut, and the
// partial character is dropped rather than written.
bool cutUtf8(std::string& s, std::size_t max)
{
    if (s.size() <= max)
        return false;
    std::size_t end = max;
    while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
        --end;
    s.resize(end);
    return true;
}

} // namespace

std::string oneLine(std::string s, std::size_t max)
{
    const std::size_t nl = s.find_first_of("\r\n");
    bool              cut = false;
    if (nl != std::string::npos) {
        s.resize(nl);
        cut = true;
    }

    if (cutUtf8(s, max))
        cut = true;

    if (cut)
        s += " ...";
    return s;
}

std::string toolSummary(const std::string& name, const json::Value* input)
{
    std::string s;
    if (name == "Bash" || name == "PowerShell") {
        s = stringField(input, "command");
    } else if (name == "Read" || name == "Edit" || name == "Write" || name == "MultiEdit") {
        s = stringField(input, "file_path");
    } else if (name == "NotebookEdit") {
        s = stringField(input, "notebook_path");
    } else if (name == "Grep" || name == "Glob") {
        s = stringField(input, "pattern");
        const std::string path = stringField(input, "path");
        if (!s.empty() && !path.empty())
            s += " in " + path;
    }

    if (s.empty())
        s = stringField(input, "description");

    // Anything else, including every MCP tool: the first non-empty string it
    // was given. A case per tool would be a list nobody can keep current, and
    // the first string is almost always the one that says what is happening.
    if (s.empty() && input && input->type == json::Value::Type::Object) {
        for (const auto& [key, value] : input->object) {
            if (value.type == json::Value::Type::String && !value.string.empty()) {
                s = value.string;
                break;
            }
        }
    }

    return oneLine(std::move(s), kSummaryChars);
}

std::string toolText(const std::string& name, const json::Value* input, std::size_t max)
{
    std::string s;
    if (name == "Bash" || name == "PowerShell") {
        s = stringField(input, "command");
    } else if (name == "Read" || name == "Edit" || name == "Write" || name == "MultiEdit") {
        s = stringField(input, "file_path");
    } else if (name == "NotebookEdit") {
        s = stringField(input, "notebook_path");
    } else if (name == "Grep" || name == "Glob") {
        s = stringField(input, "pattern");
        const std::string path = stringField(input, "path");
        if (!s.empty() && !path.empty())
            s += " in " + path;
    } else if (name == "WebFetch") {
        s = stringField(input, "url");
    } else if (name == "WebSearch") {
        s = stringField(input, "query");
    }

    if (s.empty() && input && !input->isNull())
        s = json::dump(*input);

    // A CRLF is one break, so it becomes one space rather than two.
    std::string flat;
    flat.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
            continue;
        flat += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;
    }
    cutUtf8(flat, max);
    return flat;
}

} // namespace pm::claude
