#include "agent_board.h"

#include "json.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pm::gui {
namespace {

fs::path knownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) {
        if (raw)
            CoTaskMemFree(raw);
        return {};
    }
    fs::path out(raw);
    CoTaskMemFree(raw);
    return out;
}

// ----------------------------------------------------------------- reading

// FILETIME counts 100ns ticks from 1601; Unix time counts from 1970.
constexpr std::int64_t kFileTimeUnixEpoch = 116444736000000000LL;

// The log rolls at 4MB and the board reads the live file plus one back-file,
// so this bound is never reached in normal running. It exists for the case
// where the roll did not happen: a log that grew to a gigabyte should cost the
// board its oldest events, not the refresh.
constexpr std::int64_t kMaxEventBytes = 8 * 1024 * 1024;

// A registry record is about 600 bytes. Anything near this is not one.
constexpr std::int64_t kMaxRegistryBytes = 256 * 1024;

// An agent meta file is a couple of hundred bytes.
constexpr std::int64_t kMaxMetaBytes = 64 * 1024;

// A workflow summary carries the entire script it ran. The largest here is
// 52KB and a script has no size limit of its own, so this is a bound on what
// the board will parse rather than a size anything is expected to reach.
constexpr std::int64_t kMaxWorkflowBytes = 4 * 1024 * 1024;

// One session that spawned more subagents than this has outrun what a card can
// usefully say, and the cache stops being a cache. Every run here fits inside
// it by two orders of magnitude; the largest ever seen was 173.
constexpr std::size_t kMaxCachedAgents = 20000;

// The tail of a file, read without taking it away from whoever is writing it.
//
// Sessions append to the event log continuously and Claude Code rewrites a
// registry record on every status change. An exclusive open would fail against
// a live writer, or worse, block a hook that sits on the critical path of
// someone's tool call. FILE_SHARE_DELETE is in the set for the rollover, which
// renames the live log out from under any reader holding it open.
std::string readShared(const fs::path& file, std::int64_t maxBytes)
{
    HANDLE h = CreateFileW(file.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return {};

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0) {
        CloseHandle(h);
        return {};
    }

    const std::int64_t from = size.QuadPart > maxBytes ? size.QuadPart - maxBytes : 0;
    LARGE_INTEGER      seek{};
    seek.QuadPart = from;
    if (from > 0 && !SetFilePointerEx(h, seek, nullptr, FILE_BEGIN)) {
        CloseHandle(h);
        return {};
    }

    const std::size_t want = static_cast<std::size_t>(size.QuadPart - from);
    std::string       text(want, '\0');

    std::size_t have = 0;
    while (have < want) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(want - have, 1u << 20));
        DWORD       got   = 0;
        if (!ReadFile(h, text.data() + have, chunk, &got, nullptr) || got == 0)
            break;
        have += got;
    }
    CloseHandle(h);
    text.resize(have);

    // Starting partway in lands mid-record. That first fragment is not a line.
    if (from > 0) {
        const std::size_t nl = text.find('\n');
        text = (nl == std::string::npos) ? std::string() : text.substr(nl + 1);
    }
    return text;
}

// ------------------------------------------------------------------- fields

std::string stringField(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::String) ? v->string : std::string();
}

// For a key whose shape is not pinned down. The registry is undocumented and
// carries the CLI version that wrote it, so status and waitingFor are strings
// on 2.1.270 and need not be on the next one. A number or an object comes back
// as its JSON rather than as nothing, which puts an unrecognised value on the
// card instead of letting it read as absent. Keys read as structure rather
// than as text, sessionId and cwd, still go through stringField.
std::string anyString(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    if (!v || v->isNull())
        return {};
    if (v->type == json::Value::Type::String)
        return v->string;
    return json::dump(*v);
}

std::int64_t intField(const json::Value* object, const char* key)
{
    if (!object)
        return 0;
    const json::Value* v = object->find(key);
    if (!v || v->type != json::Value::Type::Number)
        return 0;
    return v->isInteger ? v->integer : static_cast<std::int64_t>(v->number);
}

// procStart, a process creation FILETIME written as a decimal string because
// the value is past what a double holds exactly. Read it either way: a later
// writer emitting it as a number would still be reporting the same thing.
unsigned long long ticksField(const json::Value* object, const char* key)
{
    if (!object)
        return 0;
    const json::Value* v = object->find(key);
    if (!v)
        return 0;

    if (v->type == json::Value::Type::String) {
        char*                    end = nullptr;
        const unsigned long long n   = std::strtoull(v->string.c_str(), &end, 10);
        return (end && *end == '\0') ? n : 0;
    }
    if (v->type == json::Value::Type::Number && v->isInteger && v->integer > 0)
        return static_cast<unsigned long long>(v->integer);
    return 0;
}

// --------------------------------------------------------- session files

// The write time of a file, as Unix milliseconds. Zero when it cannot be read.
//
// A meta file is written once, when its subagent spawns, so its own timestamp
// is when that subagent started. Nothing inside it says so.
std::int64_t fileWriteTimeMs(const fs::path& file)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &data))
        return 0;

    ULARGE_INTEGER t{};
    t.LowPart  = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    if (t.QuadPart < static_cast<unsigned long long>(kFileTimeUnixEpoch))
        return 0;
    return (static_cast<std::int64_t>(t.QuadPart) - kFileTimeUnixEpoch) / 10000;
}

// A cwd as Claude Code spells the directory it keeps that session's files in:
// every character that is not a letter or a digit becomes a dash, and runs are
// not collapsed, so "C:\p\My App" is "C--p-My-App".
//
// Checked against all 28 project directories on this machine by reading each
// transcript's own cwd back and recomputing the name: 28 matches, 0 misses.
// It is not invertible, since a project whose name contains a dash spells the
// same as one containing a space, so this only ever goes cwd to slug.
//
// If a later Claude Code changes the rule, the cost is the subagent lines on a
// card. Everything the board already showed is read from somewhere else.
std::string projectSlug(const fs::path& cwd)
{
    std::string s = narrow(cwd.wstring());
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/'))
        s.pop_back();

    for (char& c : s) {
        const bool alnum = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                        || (c >= '0' && c <= '9');
        if (!alnum)
            c = '-';
    }
    return s;
}

// "agent-a06be1cc93c314534.meta.json" holds exactly the agent_id a hook
// reports, so the file source and the hook source join on it with no mapping.
std::string agentIdFromMeta(const fs::path& file)
{
    static constexpr std::string_view kPrefix = "agent-";
    static constexpr std::string_view kSuffix = ".meta.json";

    const std::string name = narrow(file.filename().wstring());
    if (name.size() <= kPrefix.size() + kSuffix.size())
        return {};
    if (name.compare(0, kPrefix.size(), kPrefix) != 0)
        return {};
    if (name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0)
        return {};

    return name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());
}

// "projectman-board-fixes-wf_a3f3cbaa-9df.js" is the run's name and its id
// joined by a dash, and both halves hold dashes of their own, so the split is
// on the last "-wf_" rather than the first dash.
bool splitScriptName(const fs::path& file, std::string* name, std::string* runId)
{
    static constexpr std::string_view kJoin = "-wf_";

    const std::string stem = narrow(file.stem().wstring());
    const std::size_t at   = stem.rfind(kJoin);
    if (at == std::string::npos || at == 0)
        return false;

    *name  = stem.substr(0, at);
    *runId = stem.substr(at + 1);   // keeps the "wf_", which is the id's own prefix
    return !name->empty() && runId->size() > 3;
}

// One agent-<id>.meta.json. Written at spawn and never rewritten, which is why
// nothing here reports a state: see AgentState in the header.
SubAgent readAgentMeta(const fs::path& file, std::string id, std::string runId)
{
    SubAgent a;
    a.id             = std::move(id);
    a.workflowRun    = std::move(runId);
    a.startedAtMs    = fileWriteTimeMs(file);
    a.lastActivityMs = a.startedAtMs;

    json::Value doc;
    std::string error;
    const std::string text = readShared(file, kMaxMetaBytes);
    if (!json::parse(text, &doc, &error) || doc.type != json::Value::Type::Object)
        return a;   // the id and the spawn time still make a line worth showing

    a.type        = anyString(&doc, "agentType");
    a.description = anyString(&doc, "description");
    a.phase       = anyString(&doc, "workflowPhase");
    return a;
}

// -------------------------------------------------------------- still alive

// Whether the pid in a registry record still belongs to the session that wrote
// it. Two separate questions, and the second is the one that bites: the
// registry keeps files for processes that have exited, and Windows hands a pid
// straight back out, so a stale file can name a pid that something unrelated
// now owns. The record carries the creation time that pid had when it was
// written, which is what tells the two apart.
bool processStillOurs(unsigned long pid, unsigned long long recordedTicks,
                      std::int64_t startedAtMs)
{
    if (pid == 0)
        return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc)
        return false;

    FILETIME created{}, exited{}, kernel{}, user{};
    const bool got = GetProcessTimes(proc, &created, &exited, &kernel, &user) != 0;
    CloseHandle(proc);

    // The handle opened, so something is running under that pid. Without a
    // creation time there is nothing left to check it against.
    if (!got)
        return true;

    ULARGE_INTEGER createdTicks{};
    createdTicks.LowPart  = created.dwLowDateTime;
    createdTicks.HighPart = created.dwHighDateTime;

    if (recordedTicks != 0)
        return createdTicks.QuadPart == recordedTicks;

    // No procStart, so fall back on when the record says the session started.
    // A pid reissued after that session exited belongs to a process created
    // later than the record was written. The slack covers the second or so
    // between the process starting and Claude Code writing the file.
    if (startedAtMs > 0) {
        constexpr std::int64_t kSlackMs = 60 * 1000;
        const std::int64_t     createdMs =
            (static_cast<std::int64_t>(createdTicks.QuadPart) - kFileTimeUnixEpoch) / 10000;
        return createdMs <= startedAtMs + kSlackMs;
    }
    return true;
}

// --------------------------------------------------------------- path names

// Case-folded, backslash-separated, no trailing separator. The same directory
// arrives spelled three ways here: from ProjectMan's own scan, from a hook
// payload, and from Claude Code's registry. None of the three agree on case or
// on which slash they use.
std::string pathKey(const fs::path& p)
{
    std::string s = toLower(narrow(p.wstring()));
    std::replace(s.begin(), s.end(), '/', '\\');
    while (s.size() > 1 && s.back() == '\\')
        s.pop_back();
    return s;
}

// True when `cwdKey` is `projectKey` or sits under it. The separator test is
// what stops C:\p\forge claiming a session in C:\p\forgery.
bool underPath(std::string_view cwdKey, std::string_view projectKey)
{
    if (projectKey.empty() || cwdKey.size() < projectKey.size())
        return false;
    if (cwdKey.compare(0, projectKey.size(), projectKey) != 0)
        return false;
    return cwdKey.size() == projectKey.size() || cwdKey[projectKey.size()] == '\\';
}

// The folder's own name, for a path that may or may not end in a separator.
std::string leafName(const fs::path& p)
{
    if (p.empty())
        return {};
    fs::path leaf = p.filename();
    if (leaf.empty())
        leaf = p.parent_path().filename();
    return narrow(leaf.wstring());
}

struct ProjectKey {
    std::string key;
    std::string display;
};

// Longest first, so the first key that matches a cwd is the most specific
// project containing it. A session's cwd is usually a subdirectory rather than
// the project root, and a nested checkout inside a container must win over the
// container it sits in.
std::vector<ProjectKey> projectKeys(const ProjectList& projects)
{
    std::vector<ProjectKey> keys;
    keys.reserve(projects.size());
    for (const Project& p : projects) {
        std::string k = pathKey(p.path);
        if (k.empty())
            continue;
        keys.push_back({ std::move(k), p.displayName() });
    }
    std::sort(keys.begin(), keys.end(), [](const ProjectKey& a, const ProjectKey& b) {
        return a.key.size() > b.key.size();
    });
    return keys;
}

std::string projectNameFor(const fs::path& cwd, const std::vector<ProjectKey>& keys)
{
    const std::string key = pathKey(cwd);
    if (key.empty())
        return {};
    for (const ProjectKey& p : keys) {
        if (underPath(key, p.key))
            return p.display;
    }
    return {};
}

// ------------------------------------------------------------- the fold

// A card under construction, plus the two facts that decide its column and do
// not survive onto the card itself.
struct Building {
    AgentCard   card;
    bool        busy = false;
    bool        waiting = false;
    std::string waitingFor;

    // A Notification that nothing has answered yet, and what it asked for. Both
    // are set when one arrives and cleared by the events that mean someone
    // dealt with it, which is why the text is here rather than on the card: a
    // question that has been answered is not one the card should still be
    // asking, and one written straight onto the card outlives the flag that
    // says it is live.
    bool        notifying = false;
    std::string notice;

    // Subagents by agent_id, so an event and a meta file can arrive in either
    // order and land on the same one. The card's vector is built from this at
    // the end, once there is something to sort by.
    std::unordered_map<std::string, SubAgent> agents;
};

// The subagent an event belongs to, made if this is the first sign of it.
SubAgent& agentFor(Building& b, const AgentEvent& ev)
{
    SubAgent& a = b.agents[ev.agentId];
    if (a.id.empty())
        a.id = ev.agentId;
    if (a.type.empty())
        a.type = ev.agentType;
    a.lastActivityMs = std::max(a.lastActivityMs, ev.tsMs);
    return a;
}

std::int64_t registryActivity(const RegistryEntry& e)
{
    return std::max(e.updatedAtMs, e.statusUpdatedAtMs);
}

void applyRegistry(Building& b, const RegistryEntry& e)
{
    b.card.live         = true;
    b.card.pid          = e.pid;
    b.card.startedAtMs  = e.startedAtMs;
    b.busy              = e.busy();
    b.waiting           = e.waiting();
    b.waitingFor        = e.waitingFor;

    if (!e.cwd.empty())
        b.card.cwd = e.cwd;
    if (!e.name.empty())
        b.card.name = e.name;

    b.card.lastActivityMs = std::max(b.card.lastActivityMs, registryActivity(e));
}

// The tool line an event describes: "Edit  src/gui/agent_board.cpp".
std::string toolLine(const AgentEvent& ev)
{
    std::string line = ev.tool;
    if (ev.detail.empty())
        return line;
    if (!line.empty())
        line += "  ";
    return line + ev.detail;
}

// The session moved on, so whatever it stopped to ask has been dealt with.
// Both halves of the question go together: the flag decides the column and the
// text is what the card says while it is there.
void answered(Building& b)
{
    b.notifying = false;
    b.notice.clear();
}

void applyEvent(Building& b, const AgentEvent& ev)
{
    if (b.card.cwd.empty())
        b.card.cwd = ev.cwd;
    b.card.lastActivityMs = std::max(b.card.lastActivityMs, ev.tsMs);

    // A subagent's tool calls carry its parent's session_id and nothing else to
    // tell them apart, so anything naming an agent is filed under that agent
    // and never touches the card's own line. Without this, a session running
    // five subagents shows whichever of them called a tool last as what the
    // session itself is doing, which is the one thing the card claims to say.
    if (!ev.agentId.empty()) {
        SubAgent& a = agentFor(b, ev);
        if (ev.event == "SubagentStart") {
            a.state       = AgentState::Running;
            a.startedAtMs = ev.tsMs;
        } else if (ev.event == "SubagentStop") {
            a.state = AgentState::Finished;
        } else if (ev.event == "PreToolUse") {
            if (std::string line = toolLine(ev); !line.empty())
                a.activity = std::move(line);
            // A tool call is proof it was running at the time, and the log is
            // walked forward, so a later Stop still wins.
            if (a.state == AgentState::Spawned)
                a.state = AgentState::Running;
        }
        return;
    }

    // The log is in append order and no two sessions can reorder each other's
    // lines, so walking it forward and letting the last write win is the same
    // as reading one session's events newest first.
    //
    // A detail is omitted rather than written empty when there is nothing to
    // say, which is indistinguishable here from a detail that was lost. Either
    // way, blanking what the card already shows gains nothing.
    if (ev.event == "PreToolUse") {
        if (std::string line = toolLine(ev); !line.empty())
            b.card.activity = std::move(line);
        answered(b);
    } else if (ev.event == "UserPromptSubmit") {
        if (!ev.detail.empty())
            b.card.prompt = ev.detail;
        answered(b);
    } else if (ev.event == "Notification") {
        b.notice    = ev.detail;
        b.notifying = true;
    } else if (ev.event == "Stop") {
        answered(b);
    }
}

// The file source names what a subagent is; the hooks say what it is doing. An
// id seen only here therefore has a description and no state, which is exactly
// what AgentState::Spawned means. Neither source overwrites the other: whoever
// had something to say first keeps it.
void applyFiles(Building& b, const SessionFiles& f)
{
    for (const SubAgent& meta : f.agents) {
        SubAgent& a = b.agents[meta.id];
        if (a.id.empty())
            a.id = meta.id;
        if (a.type.empty())
            a.type = meta.type;
        if (a.description.empty())
            a.description = meta.description;
        if (a.workflowRun.empty())
            a.workflowRun = meta.workflowRun;
        if (a.phase.empty())
            a.phase = meta.phase;
        if (a.startedAtMs == 0)
            a.startedAtMs = meta.startedAtMs;

        a.lastActivityMs = std::max(a.lastActivityMs, meta.startedAtMs);
    }

    b.card.workflows = f.workflows;
}

// Where the two sources meet.
//
// `dead` is a session whose process is gone. Nothing it started is still
// running, whatever the last event said, and the same holds one level down: a
// run that has ended is proof that every agent it spawned has ended, which is
// the only way a subagent seen on disk alone ever leaves Spawned.
void foldAgents(Building& b, bool dead)
{
    std::unordered_map<std::string, std::size_t> runs;
    for (std::size_t i = 0; i < b.card.workflows.size(); ++i)
        runs.emplace(b.card.workflows[i].runId, i);

    // The phase of the most recently spawned agent, which is as close as either
    // file gets to saying which phase a run has reached.
    std::vector<std::int64_t> phaseAt(b.card.workflows.size(), 0);

    b.card.agents.reserve(b.agents.size());
    for (auto& [id, agent] : b.agents) {
        const auto  at  = runs.find(agent.workflowRun);
        WorkflowRun* run = (at == runs.end()) ? nullptr : &b.card.workflows[at->second];

        if (dead || (run && run->finished()))
            agent.state = AgentState::Finished;

        if (run) {
            ++run->spawned;
            if (agent.state == AgentState::Running)
                ++run->running;
            if (!agent.phase.empty() && agent.startedAtMs >= phaseAt[at->second]) {
                phaseAt[at->second] = agent.startedAtMs;
                run->phase          = agent.phase;
            }
        }

        b.card.agents.push_back(std::move(agent));
    }

    // Running first, because a card is read for what is happening now, then
    // most recently active. The id breaks the tie so an unordered_map's own
    // order never reaches the screen.
    std::sort(b.card.agents.begin(), b.card.agents.end(),
              [](const SubAgent& a, const SubAgent& c) {
                  const bool ra = a.state == AgentState::Running;
                  const bool rc = c.state == AgentState::Running;
                  if (ra != rc)
                      return ra;
                  if (a.lastActivityMs != c.lastActivityMs)
                      return a.lastActivityMs > c.lastActivityMs;
                  return a.id < c.id;
              });

    std::sort(b.card.workflows.begin(), b.card.workflows.end(),
              [](const WorkflowRun& a, const WorkflowRun& c) {
                  if (a.finished() != c.finished())
                      return c.finished();
                  if (a.startedAtMs != c.startedAtMs)
                      return a.startedAtMs > c.startedAtMs;
                  return a.runId < c.runId;
              });
}

int columnRank(AgentColumn c)
{
    const std::vector<AgentColumn>& order = columnOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == c)
            return static_cast<int>(i);
    }
    return static_cast<int>(order.size());
}

} // namespace

// ------------------------------------------------------------------ columns

const char* columnLabel(AgentColumn c)
{
    switch (c) {
    case AgentColumn::NeedsYou: return "NEEDS YOU";
    case AgentColumn::Working:  return "WORKING";
    case AgentColumn::Idle:     return "IDLE";
    case AgentColumn::Done:     return "DONE";
    }
    return "";
}

int AgentCard::looseAgents(AgentState state) const
{
    int n = 0;
    for (const SubAgent& a : agents) {
        if (a.workflowRun.empty() && a.state == state)
            ++n;
    }
    return n;
}

const std::vector<AgentColumn>& columnOrder()
{
    static const std::vector<AgentColumn> order = {
        AgentColumn::NeedsYou,
        AgentColumn::Working,
        AgentColumn::Idle,
        AgentColumn::Done,
    };
    return order;
}

// ------------------------------------------------------------------ sources

std::vector<RegistryEntry> readRegistry(const fs::path& sessionsDir)
{
    std::vector<RegistryEntry> out;
    if (sessionsDir.empty())
        return out;

    std::error_code ec;
    if (!fs::is_directory(sessionsDir, ec))
        return out;

    for (const auto& entry : fs::directory_iterator(sessionsDir, ec)) {
        if (ec)
            break;

        // Every record is <pid>.json. The .key files beside them are the
        // session's messaging secret and are none of the board's business.
        if (entry.path().extension() != ".json")
            continue;

        const std::string text = readShared(entry.path(), kMaxRegistryBytes);
        if (text.empty())
            continue;

        json::Value doc;
        std::string error;
        if (!json::parse(text, &doc, &error) || doc.type != json::Value::Type::Object)
            continue;

        RegistryEntry e;
        e.pid               = static_cast<unsigned long>(intField(&doc, "pid"));
        e.sessionId         = stringField(&doc, "sessionId");
        e.name              = anyString(&doc, "name");
        e.status            = anyString(&doc, "status");
        e.waitingFor        = anyString(&doc, "waitingFor");
        e.kind              = anyString(&doc, "kind");
        e.entrypoint        = anyString(&doc, "entrypoint");
        e.version           = anyString(&doc, "version");
        e.startedAtMs       = intField(&doc, "startedAt");
        e.updatedAtMs       = intField(&doc, "updatedAt");
        e.statusUpdatedAtMs = intField(&doc, "statusUpdatedAt");

        const std::string cwd = stringField(&doc, "cwd");
        if (!cwd.empty())
            e.cwd = fs::path(widen(cwd));

        if (!processStillOurs(e.pid, ticksField(&doc, "procStart"), e.startedAtMs))
            continue;

        out.push_back(std::move(e));
    }
    return out;
}

std::vector<AgentEvent> readEvents(const fs::path& eventLog, std::int64_t sinceMs)
{
    std::vector<AgentEvent> out;
    if (eventLog.empty())
        return out;

    // events.jsonl rolls to events.1.jsonl beside it, so the back-file holds
    // everything older than the live one and is read first. src/cli/hook.h
    // owns that naming and the two must agree.
    const fs::path rolled = eventLog.parent_path()
                          / (eventLog.stem().wstring() + L".1" + eventLog.extension().wstring());

    for (const fs::path& file : { rolled, eventLog }) {
        const std::string text = readShared(file, kMaxEventBytes);
        if (text.empty())
            continue;

        for (const std::string_view raw : splitLines(text)) {
            const std::string_view line = trim(raw);
            if (line.empty())
                continue;

            // A line torn in half by a session that was killed mid-append, or
            // one caught between the write and the newline. Skipping it costs
            // a card some detail; failing the read would cost the whole board
            // every event behind it.
            json::Value doc;
            std::string error;
            if (!json::parse(line, &doc, &error) || doc.type != json::Value::Type::Object)
                continue;

            AgentEvent e;
            e.tsMs = intField(&doc, "ts");
            if (sinceMs > 0 && e.tsMs < sinceMs)
                continue;

            e.event     = stringField(&doc, "event");
            e.sessionId = stringField(&doc, "session");
            e.tool      = stringField(&doc, "tool");
            e.detail    = stringField(&doc, "detail");
            e.agentId   = stringField(&doc, "agent");
            e.agentType = stringField(&doc, "agentType");
            e.toolUseId = stringField(&doc, "toolUse");

            const std::string cwd = stringField(&doc, "cwd");
            if (!cwd.empty())
                e.cwd = fs::path(widen(cwd));

            out.push_back(std::move(e));
        }
    }
    return out;
}

// ------------------------------------------------------------ session files

const fs::path& SessionFileReader::directoryFor(const std::string& sessionId,
                                                const fs::path&    cwd)
{
    static const fs::path kNone;

    if (const auto it = dirs_.find(sessionId); it != dirs_.end())
        return it->second;

    if (sessionId.empty() || cwd.empty())
        return kNone;

    const fs::path root = sessionDataRoot();
    if (root.empty())
        return kNone;

    // The directory appears the first time a session spawns something, so a
    // miss here is the ordinary state of a session that has not, and is not
    // cached: the next refresh asks again and costs one is_directory call.
    std::error_code ec;
    fs::path        dir = root / widen(projectSlug(cwd)) / widen(sessionId);
    if (!fs::is_directory(dir, ec))
        return kNone;

    return dirs_.emplace(sessionId, std::move(dir)).first->second;
}

SessionFiles SessionFileReader::read(const std::string& sessionId, const fs::path& cwd)
{
    SessionFiles out;
    out.sessionId = sessionId;

    const fs::path dir = directoryFor(sessionId, cwd);
    if (dir.empty())
        return out;

    // The cache is what keeps this from being quadratic in a session's own
    // history. Past the bound it is not helping any more, and holding it would
    // just be a leak with a cache's name on it.
    if (metas_.size() > kMaxCachedAgents)
        metas_.clear();

    const fs::path subagents = dir / "subagents";

    // Loose subagents first, then one directory per workflow run. Both hold the
    // same meta files; only the run directory says which run they belong to.
    const auto collect = [&](const fs::path& from, const std::string& runId) {
        std::error_code listing;
        if (!fs::is_directory(from, listing))
            return;

        for (const auto& entry : fs::directory_iterator(from, listing)) {
            if (listing)
                break;

            const std::string id = agentIdFromMeta(entry.path());
            if (id.empty())
                continue;

            auto it = metas_.find(id);
            if (it == metas_.end())
                it = metas_.emplace(id, readAgentMeta(entry.path(), id, runId)).first;

            out.agents.push_back(it->second);
        }
    };

    collect(subagents, {});

    std::error_code ec;
    const fs::path  runs = subagents / "workflows";
    if (fs::is_directory(runs, ec)) {
        for (const auto& entry : fs::directory_iterator(runs, ec)) {
            if (ec)
                break;
            if (entry.is_directory(ec))
                collect(entry.path(), narrow(entry.path().filename().wstring()));
        }
    }

    // A run is live from the moment its script lands and stays that way until
    // the summary appears beside it, which is the only thing that ever carries
    // a status. See the header: the summary is written at the end of the run,
    // not during it.
    const fs::path workflows = dir / "workflows";
    const fs::path scripts   = workflows / "scripts";

    std::error_code listing;
    if (!fs::is_directory(scripts, listing))
        return out;

    for (const auto& entry : fs::directory_iterator(scripts, listing)) {
        if (listing)
            break;

        WorkflowRun run;
        if (!splitScriptName(entry.path(), &run.name, &run.runId))
            continue;

        run.status      = "running";
        run.startedAtMs = fileWriteTimeMs(entry.path());

        if (const auto it = finished_.find(run.runId); it != finished_.end()) {
            out.workflows.push_back(it->second);
            continue;
        }

        json::Value doc;
        std::string error;
        const std::string text =
            readShared(workflows / widen(run.runId + ".json"), kMaxWorkflowBytes);

        if (json::parse(text, &doc, &error) && doc.type == json::Value::Type::Object) {
            const std::string status = anyString(&doc, "status");
            if (!status.empty())
                run.status = status;
            if (const std::int64_t started = intField(&doc, "startTime"); started > 0)
                run.startedAtMs = started;

            // Terminal, so it cannot change again and the summary need never be
            // parsed twice. It carries the whole script it ran, tens of
            // kilobytes of it, and re-reading that every few seconds for every
            // run a long session ever started is the one expensive thing here.
            if (run.finished())
                finished_.emplace(run.runId, run);
        }

        out.workflows.push_back(std::move(run));
    }

    return out;
}

// -------------------------------------------------------------------- board

AgentList buildBoard(const std::vector<RegistryEntry>& registry,
                     const std::vector<AgentEvent>&    events,
                     const std::vector<SessionFiles>&  files,
                     const ProjectList&                projects)
{
    std::vector<Building>                        build;
    std::unordered_map<std::string, std::size_t> byKey;
    build.reserve(registry.size() + 8);

    // The registry first: it is the only source that knows a process exists,
    // so a card that starts here is one the board can still act on.
    for (const RegistryEntry& e : registry) {
        // A record with no sessionId cannot be joined to its events, but it is
        // still a running agent and dropping it would hide exactly what the
        // board is for. One file per pid, and readRegistry has already proved
        // the pid live, so the pid keys it without colliding.
        const std::string key =
            e.sessionId.empty() ? ("pid:" + std::to_string(e.pid)) : e.sessionId;

        const auto [it, inserted] = byKey.try_emplace(key, build.size());
        if (inserted) {
            build.emplace_back();
            build.back().card.sessionId = e.sessionId;
        } else if (registryActivity(e) < build[it->second].card.lastActivityMs) {
            // Two live processes claiming one session id, which is a resume
            // that kept the id while the old process was winding down. The
            // record that moved most recently is the one to show.
            continue;
        }
        applyRegistry(build[it->second], e);
    }

    // Then the events. A session id with no registry entry has no process left
    // behind it, so it arrives here already destined for Done.
    for (const AgentEvent& ev : events) {
        if (ev.sessionId.empty())
            continue;

        const auto [it, inserted] = byKey.try_emplace(ev.sessionId, build.size());
        if (inserted) {
            build.emplace_back();
            build.back().card.sessionId = ev.sessionId;
        }
        applyEvent(build[it->second], ev);
    }

    // Last, and only onto cards that already exist. A session's directory
    // outlives the session, so a directory full of meta files is not evidence
    // that anything is running and must never conjure a card of its own: the
    // registry and the event log between them already know every session worth
    // showing.
    for (const SessionFiles& f : files) {
        if (f.sessionId.empty())
            continue;
        if (const auto it = byKey.find(f.sessionId); it != byKey.end())
            applyFiles(build[it->second], f);
    }

    const std::vector<ProjectKey> keys = projectKeys(projects);

    AgentList cards;
    cards.reserve(build.size());

    for (Building& b : build) {
        AgentCard& c = b.card;

        // First match wins. One chain rather than three independent flags,
        // because the flags disagree constantly: a session sitting on a
        // permission prompt still reports itself busy, and the loudest true
        // thing is the one worth putting on the card.
        //
        // Every rung above Done needs a live registry entry. A Notification is
        // the only thing the event log can say about a column, and it can only
        // ever say it about a session that is still there to answer: a hook
        // cannot report that its session died, so a question left hanging by a
        // killed session is not a question anyone can answer.
        // A "waiting" status counts on its own, not only through waitingFor.
        // The two travel together on every sample seen, but they are separate
        // fields written by something this does not control, and a session that
        // says it is waiting belongs in front of someone whether or not it also
        // said what for.
        if (c.live && (b.notifying || b.waiting || !b.waitingFor.empty()))
            c.column = AgentColumn::NeedsYou;
        else if (c.live && b.busy)
            c.column = AgentColumn::Working;
        else if (c.live)
            c.column = AgentColumn::Idle;
        else
            c.column = AgentColumn::Done;

        // Only a card in NeedsYou has something to say here, and it says it in
        // red, so a line left over from a question that was answered is a card
        // shouting for someone who is no longer needed. The notice is what the
        // Notification asked for while it stands; waitingFor is the registry's
        // own account of what it wants, and it is all there is when the column
        // was decided without a Notification.
        if (c.column == AgentColumn::NeedsYou)
            c.attention = (b.notifying && !b.notice.empty()) ? b.notice : b.waitingFor;
        if (c.name.empty())
            c.name = leafName(c.cwd);

        c.project = projectNameFor(c.cwd, keys);

        foldAgents(b, c.column == AgentColumn::Done);

        cards.push_back(std::move(c));
    }

    // Grouped by column in display order, then most recently active first. The
    // session id breaks the tie so a refresh that changed nothing redraws the
    // board in the order it was already in.
    std::sort(cards.begin(), cards.end(), [](const AgentCard& a, const AgentCard& b) {
        const int ra = columnRank(a.column);
        const int rb = columnRank(b.column);
        if (ra != rb)
            return ra < rb;
        if (a.lastActivityMs != b.lastActivityMs)
            return a.lastActivityMs > b.lastActivityMs;
        return a.sessionId < b.sessionId;
    });

    return cards;
}

// ------------------------------------------------------------------ locations

fs::path eventLogPath()
{
    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        return {};
    return local / "ProjectMan" / "agents" / "events.jsonl";
}

fs::path sessionRegistryDir()
{
    const fs::path profile = knownFolder(FOLDERID_Profile);
    if (profile.empty())
        return {};
    return profile / ".claude" / "sessions";
}

fs::path sessionDataRoot()
{
    const fs::path profile = knownFolder(FOLDERID_Profile);
    if (profile.empty())
        return {};
    return profile / ".claude" / "projects";
}

} // namespace pm::gui
