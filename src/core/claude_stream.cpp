#include "claude_stream.h"

#include "json.h"

#include <cstdio>

namespace pm::claude {
namespace {

constexpr size_t kSummaryChars = 120;
constexpr size_t kErrorChars   = 2000;

std::string stringField(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::String) ? v->string : std::string();
}

double numberField(const json::Value* object, const char* key)
{
    if (!object)
        return 0;
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::Number) ? v->number : 0;
}

bool boolField(const json::Value* object, const char* key)
{
    if (!object)
        return false;
    const json::Value* v = object->find(key);
    return v && v->type == json::Value::Type::Bool && v->boolean;
}

// First line only, cut to fit, with a marker for what was cut.
std::string oneLine(std::string s, size_t max)
{
    const size_t nl = s.find_first_of("\r\n");
    bool cut = false;
    if (nl != std::string::npos) {
        s.resize(nl);
        cut = true;
    }
    if (s.size() > max) {
        s.resize(max);
        cut = true;
    }
    if (cut)
        s += " ...";
    return s;
}

} // namespace

// Declared in the header, so it sits outside the anonymous namespace above. The
// helpers it uses are still reachable: an anonymous namespace's members are
// visible in the namespace enclosing it.
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

namespace {

// A tool result's content is a string, or an array of blocks each with text.
std::string resultText(const json::Value* content)
{
    if (!content)
        return {};
    if (content->type == json::Value::Type::String)
        return content->string;
    if (content->type != json::Value::Type::Array)
        return json::dump(*content);

    std::string out;
    for (const json::Value& block : content->array) {
        const std::string text = stringField(&block, "text");
        if (text.empty())
            continue;
        if (!out.empty())
            out.push_back('\n');
        out += text;
    }
    return out;
}

Event withRaw(EventKind kind, std::string_view line)
{
    Event e;
    e.kind = kind;
    e.raw  = std::string(line);
    return e;
}

} // namespace

std::vector<Event> parseLine(std::string_view line)
{
    std::vector<Event> out;

    json::Value doc;
    std::string error;
    if (!json::parse(line, &doc, &error) || doc.type != json::Value::Type::Object) {
        Event e = withRaw(EventKind::Malformed, line);
        e.text  = e.raw;
        out.push_back(std::move(e));
        return out;
    }

    const std::string type      = stringField(&doc, "type");
    const std::string sessionId = stringField(&doc, "session_id");

    if (type == "system") {
        Event e     = withRaw(EventKind::System, line);
        e.subtype   = stringField(&doc, "subtype");
        e.sessionId = sessionId;
        if (e.subtype == "init") {
            e.kind         = EventKind::Init;
            e.model        = stringField(&doc, "model");
            e.apiKeySource = stringField(&doc, "apiKeySource");
        }
        out.push_back(std::move(e));
        return out;
    }

    if (type == "rate_limit_event") {
        Event e     = withRaw(EventKind::RateLimit, line);
        e.sessionId = sessionId;
        out.push_back(std::move(e));
        return out;
    }

    if (type == "result") {
        Event e              = withRaw(EventKind::Result, line);
        e.sessionId          = sessionId;
        e.subtype            = stringField(&doc, "subtype");
        e.isError            = boolField(&doc, "is_error");
        e.text               = stringField(&doc, "result");
        e.costUsd            = numberField(&doc, "total_cost_usd");
        e.durationMs         = static_cast<int>(numberField(&doc, "duration_ms"));
        e.numTurns           = static_cast<int>(numberField(&doc, "num_turns"));
        if (const json::Value* denials = doc.find("permission_denials");
            denials && denials->type == json::Value::Type::Array) {
            e.permissionDenials = static_cast<int>(denials->array.size());
        }
        out.push_back(std::move(e));
        return out;
    }

    if (type == "assistant" || type == "user") {
        const json::Value* message = doc.find("message");
        const json::Value* content = message ? message->find("content") : nullptr;

        if (!content || content->type != json::Value::Type::Array) {
            Event e     = withRaw(EventKind::Unknown, line);
            e.sessionId = sessionId;
            e.text      = e.raw;
            out.push_back(std::move(e));
            return out;
        }

        for (const json::Value& block : content->array) {
            const std::string blockType = stringField(&block, "type");

            Event e     = withRaw(EventKind::Unknown, line);
            e.sessionId = sessionId;

            if (blockType == "text") {
                e.kind = EventKind::Text;
                e.text = stringField(&block, "text");
            } else if (blockType == "thinking") {
                e.kind = EventKind::Thinking;
                e.text = stringField(&block, "thinking");
            } else if (blockType == "tool_use") {
                e.kind        = EventKind::ToolUse;
                e.toolName    = stringField(&block, "name");
                e.toolSummary = toolSummary(e.toolName, block.find("input"));
            } else if (blockType == "tool_result") {
                e.kind    = EventKind::ToolResult;
                e.isError = boolField(&block, "is_error");
                e.text    = resultText(block.find("content"));
            } else {
                e.text = json::dump(block);
            }
            out.push_back(std::move(e));
        }
        return out;
    }

    Event e     = withRaw(EventKind::Unknown, line);
    e.sessionId = sessionId;
    e.text      = e.raw;
    out.push_back(std::move(e));
    return out;
}

Event stderrEvent(std::string line)
{
    Event e;
    e.kind = EventKind::Stderr;
    e.raw  = line;
    e.text = std::move(line);
    return e;
}

std::optional<RenderedLine> render(const Event& e)
{
    switch (e.kind) {
    case EventKind::Text:
        return RenderedLine{ e.text, LineStyle::Text };

    case EventKind::ToolUse:
        return RenderedLine{ "> " + e.toolName + "  " + e.toolSummary, LineStyle::Tool };

    case EventKind::ToolResult:
        if (!e.isError)
            return std::nullopt;
        return RenderedLine{ "! " + (e.text.size() > kErrorChars
                                          ? e.text.substr(0, kErrorChars) + " ..."
                                          : e.text),
                             LineStyle::Error };

    case EventKind::System:
        // A running token count, sent every few seconds. Not news.
        if (e.subtype == "thinking_tokens")
            return std::nullopt;
        return RenderedLine{ "system " + e.subtype, LineStyle::Meta };

    case EventKind::Unknown:
    case EventKind::Malformed:
        return RenderedLine{ e.text.empty() ? e.raw : e.text, LineStyle::Raw };

    case EventKind::Stderr:
        return RenderedLine{ e.text, LineStyle::Error };

    case EventKind::Init:
    case EventKind::Thinking:
    case EventKind::Result:
    case EventKind::RateLimit:
        return std::nullopt;
    }
    return std::nullopt;
}

void account(RunStats& s, const Event& e)
{
    if (s.sessionId.empty() && !e.sessionId.empty())
        s.sessionId = e.sessionId;

    switch (e.kind) {
    case EventKind::Init:
        s.model        = e.model;
        s.apiKeySource = e.apiKeySource;
        break;
    case EventKind::ToolUse:
        ++s.toolCalls;
        break;
    case EventKind::Result:
        s.sawResult = true;
        s.result    = e;
        break;
    case EventKind::Stderr:
        s.lastStderr = e.text;
        break;
    default:
        break;
    }
}

std::string describeDuration(int ms)
{
    char buf[32];
    if (ms < 60000) {
        std::snprintf(buf, sizeof(buf), "%.1f s", ms / 1000.0);
    } else {
        const int minutes = ms / 60000;
        const int seconds = (ms / 1000) % 60;
        std::snprintf(buf, sizeof(buf), "%dm %02ds", minutes, seconds);
    }
    return buf;
}

RunSummary summarize(const RunStats& s, const StreamResult& r)
{
    RunSummary out;

    if (!r.started) {
        out.state    = RunState::Failed;
        out.headline = "FAILED  " + r.launchError;
        return out;
    }

    if (r.stopped) {
        out.state    = RunState::Stopped;
        out.headline = "STOPPED  " + std::to_string(s.toolCalls)
                     + (s.toolCalls == 1 ? " tool call" : " tool calls");
        return out;
    }

    if (s.sawResult && !s.result.isError) {
        char cost[32];
        std::snprintf(cost, sizeof(cost), "$%.2f", s.result.costUsd);

        // Claude Code prices every run at API rates whether or not an API key
        // paid for it. A session signed in on a subscription drew on that
        // subscription's usage instead, so the figure is an equivalent, not a
        // charge, and is labelled as one.
        const std::string spend = s.apiKeySource == "none"
                                      ? std::string("subscription, ") + cost + " at API rates"
                                      : std::string(cost);

        out.state    = RunState::Done;
        out.headline = "DONE  " + describeDuration(s.result.durationMs) + "  "
                     + std::to_string(s.result.numTurns)
                     + (s.result.numTurns == 1 ? " turn" : " turns") + "  " + spend;
        if (s.result.permissionDenials > 0) {
            out.headline += "  " + std::to_string(s.result.permissionDenials)
                          + " permission denied";
        }
        return out;
    }

    out.state = RunState::Failed;
    if (s.sawResult) {
        out.headline = "FAILED  " + s.result.subtype;
        if (!s.result.text.empty())
            out.headline += ": " + oneLine(s.result.text, 200);
        return out;
    }

    if (r.exitCode != 0) {
        out.headline = "FAILED  exit " + std::to_string(r.exitCode);
        if (!s.lastStderr.empty())
            out.headline += "  " + oneLine(s.lastStderr, 200);
        return out;
    }

    out.headline = "FAILED  ended without a result";
    return out;
}

std::vector<std::string> describeRun(const DispatchPlan& plan, const Config& cfg)
{
    std::vector<std::string> lines;

    std::string head = std::to_string(plan.items.size())
                     + (plan.items.size() == 1 ? " item across " : " items across ")
                     + std::to_string(plan.repos.size())
                     + (plan.repos.size() == 1 ? " repository" : " repositories") + "  "
                     + autonomyLabel(cfg.autonomy);
    if (!cfg.effort.empty())
        head += "  effort " + cfg.effort;
    if (!cfg.model.empty())
        head += "  model " + cfg.model;
    lines.push_back(std::move(head));

    for (const fs::path& repo : plan.repos)
        lines.push_back("  " + repo.string());

    return lines;
}

} // namespace pm::claude
