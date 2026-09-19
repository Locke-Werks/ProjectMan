#include "agent_board.h"

#include "claude_files.h"
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

// Reading a file somebody else is still writing, and the field accessors
// that go with it. claude_files.h holds them because claude_state.cpp reads
// the same kind of file the same way; the reasoning for each is there.
using cf::anyString;
using cf::fileWriteTimeMs;
using cf::intField;
using cf::kFileTimeUnixEpoch;
using cf::processStillOurs;
using cf::projectSlug;
using cf::readShared;
using cf::stringField;
using cf::ticksField;

// ----------------------------------------------------------------- reading

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

// A run journal carries each agent's whole result, so it is the script's output
// rather than its text: 800KB for the largest here, a security review that ran
// 43 agents. Same bound for the same reason. Past it the oldest records go, and
// the agents they started are still accounted for by their meta files.
constexpr std::int64_t kMaxJournalBytes = 4 * 1024 * 1024;

// One session that spawned more subagents than this has outrun what a card can
// usefully say, and the cache stops being a cache. Every run here fits inside
// it by two orders of magnitude; the largest ever seen was 173.
constexpr std::size_t kMaxCachedAgents = 20000;

// The same, for journals. Far lower because a run holds many agents and this
// machine's entire history is 19 runs: a map past this size is one that has
// stopped being about anything still on screen.
constexpr std::size_t kMaxCachedRuns = 512;





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

// Forward only, so the source that knows more decides and the order two sources
// are read in cannot change the answer.
void promote(AgentState* state, AgentState to)
{
    if (state && *state < to)
        *state = to;
}

// One workflow run's journal.jsonl, the run's own account of its agents.
//
// It is the only thing that says what a run is doing while it is doing it: the
// summary beside it carries far more, per-agent state and last tool and all,
// and is not written until the run has ended. It is also the only source that
// says so with no hooks installed at all.
//
// Four record types on 2.1.276, none of them timestamped:
//
//   launched   the run began. Nothing else in it.
//   started    agentId, plus the label and phase the script gave that agent.
//   result     agentId, plus everything the agent returned.
//   failed     agentId, and nothing about why.
//
// `launched` appears in 9 of the 19 runs on this machine, so it arrived at some
// point and older runs do without it. Treat every type as optional, and an
// unrecognised one as something new rather than as an error: a record naming an
// agent is worth filing even when what it says is not understood.
std::vector<SubAgent> readJournal(const fs::path& file, const std::string& runId)
{
    std::vector<SubAgent> out;

    const std::string text = readShared(file, kMaxJournalBytes);
    if (text.empty())
        return out;

    // Linear, because a run's agents are counted in tens: the largest here is
    // 43, and the largest ever seen anywhere is 173.
    const auto find = [&out](const std::string& id) -> SubAgent* {
        for (SubAgent& a : out) {
            if (a.id == id)
                return &a;
        }
        return nullptr;
    };

    for (const std::string_view raw : splitLines(text)) {
        const std::string_view line = trim(raw);
        if (line.empty())
            continue;

        // A line caught between the write and the newline, or a result too
        // large for the cap to have kept whole. Skipping it costs one agent's
        // state until the next refresh reads the file again.
        json::Value doc;
        std::string error;
        if (!json::parse(line, &doc, &error) || doc.type != json::Value::Type::Object)
            continue;

        const std::string id = stringField(&doc, "agentId");
        if (id.empty())
            continue;   // "launched", and anything else about the run itself

        SubAgent* a = find(id);
        if (!a) {
            out.emplace_back();
            a              = &out.back();
            a->id          = id;
            a->workflowRun = runId;
            a->type        = "workflow-subagent";
        }

        const std::string type = stringField(&doc, "type");
        if (type == "started") {
            promote(&a->state, AgentState::Running);
            if (std::string label = anyString(&doc, "label"); !label.empty())
                a->description = std::move(label);
            if (std::string phase = anyString(&doc, "phase"); !phase.empty())
                a->phase = std::move(phase);
        } else if (type == "result") {
            promote(&a->state, AgentState::Finished);
        } else if (type == "failed") {
            promote(&a->state, AgentState::Finished);
            a->failed = true;
        }
    }
    return out;
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

// File the call on the card, newest last, oldest dropped past the cap.
//
// Keyed by tool_use_id so the same call read twice is one entry: the log is
// re-read whole on every refresh, and a call that appeared last time must not
// appear again as a second box beside itself.
void rememberTool(Building& b, const AgentEvent& ev)
{
    if (ev.toolUseId.empty() || ev.tool.empty())
        return;

    for (ToolCall& t : b.card.tools) {
        if (t.id == ev.toolUseId)
            return;
    }

    ToolCall call;
    call.id      = ev.toolUseId;
    call.tool    = ev.tool;
    call.detail  = ev.detail;
    call.agentId = ev.agentId;
    call.tsMs    = ev.tsMs;
    b.card.tools.push_back(std::move(call));

    if (b.card.tools.size() > kMaxRecentTools)
        b.card.tools.erase(b.card.tools.begin());
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
            rememberTool(b, ev);
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
        rememberTool(b, ev);
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
//
// State is the exception, and only because it cannot be written backwards:
// inside a workflow the files do know when an agent started and stopped, and
// what they know is folded in rather than dropped. A hook that saw the same
// agent finish still wins, because finishing is as far as it goes.
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

        promote(&a.state, meta.state);
        a.failed = a.failed || meta.failed;

        a.lastActivityMs = std::max(a.lastActivityMs, meta.startedAtMs);
    }

    b.card.workflows = f.workflows;
    b.card.tasks     = f.tasks;
    b.card.todos     = f.todos;
}

// A background Claude session onto a card.
//
// Lands where a registry entry would, and for the same reason: this is the only
// source that knows the process exists. `busy` rather than a column directly,
// so a job goes through the same chain every other card does and a hook event
// from inside one can still move it to NeedsYou.
void applyJob(Building& b, const BackgroundJob& job)
{
    AgentCard& c = b.card;

    c.kind      = CardKind::Background;
    c.sessionId = job.sessionId;
    c.live      = true;
    c.pid       = job.pid;

    if (!job.name.empty())
        c.name = job.name;
    if (!job.cwd.empty())
        c.cwd = job.cwd;

    // The job's own rolling line, which its timeline keeps more current than
    // any hook does: a daemon worker writes a state change per turn whether or
    // not ProjectMan's hooks are installed on the machine.
    if (!job.detail.empty())
        c.activity = job.detail;
    if (c.prompt.empty() && !job.intent.empty())
        c.prompt = job.intent;

    if (job.createdAtMs > 0)
        c.startedAtMs = job.createdAtMs;
    c.lastActivityMs = std::max(c.lastActivityMs, job.updatedAtMs);

    b.busy = b.busy || job.working();
}

// ------------------------------------------------------- the background tasks

// What opened a background task: the tool, the text it was given, who ran it,
// and when it stops mattering.
struct TaskCall {
    TaskKind    kind = TaskKind::Unknown;
    std::string label;
    std::string agentId;

    // Monitors only. The PostToolUse timestamp plus the watch's declared
    // timeout, which is when it can no longer be armed. Zero for a shell, which
    // answers the same question with its own open file handle instead.
    std::int64_t expiresAtMs = 0;
};

TaskKind kindForTool(const std::string& tool)
{
    if (tool == "Monitor")
        return TaskKind::Monitor;
    if (tool == "Bash" || tool == "PowerShell")
        return TaskKind::Shell;
    // Something else learned to open one. Unknown is a real answer and the node
    // still draws, labelled with whatever the call carried.
    return TaskKind::Unknown;
}

// Every background task the log can name, keyed by session and task id.
//
// Two passes over the events because the halves arrive in either order and
// neither is much use alone: PostToolUse carries the task id and nothing else,
// PreToolUse carries the command and cannot know the id yet. They are tied by
// tool_use_id, which is exact. Keyed by session as well because a task id is
// Claude Code's and nothing promises it is unique across sessions.
std::unordered_map<std::string, TaskCall> taskCalls(const std::vector<AgentEvent>& events)
{
    std::unordered_map<std::string, const AgentEvent*> calls;
    for (const AgentEvent& ev : events) {
        if (ev.event == "PreToolUse" && !ev.toolUseId.empty())
            calls[ev.toolUseId] = &ev;
    }

    std::unordered_map<std::string, TaskCall> out;
    for (const AgentEvent& ev : events) {
        if (ev.event != "PostToolUse" || ev.task.empty() || ev.sessionId.empty())
            continue;

        TaskCall call;
        // The PostToolUse line carries the tool name too, so a PreToolUse that
        // has rolled off the back of the log still leaves the kind known and
        // only costs the label.
        call.kind = kindForTool(ev.tool);

        // And it carries agent_id when a subagent made the call, which is the
        // only place that fact exists: the task files all share one directory
        // per session whoever opened them.
        call.agentId = ev.agentId;

        if (const auto at = calls.find(ev.toolUseId); at != calls.end()) {
            call.label = at->second->detail;
            if (call.kind == TaskKind::Unknown)
                call.kind = kindForTool(at->second->tool);

            // The deadline is counted from here rather than from the PreToolUse
            // that carries it: the watch starts when the call returns, and this
            // line IS the call returning.
            if (call.kind == TaskKind::Monitor && at->second->ttlMs > 0)
                call.expiresAtMs = ev.tsMs + at->second->ttlMs;
        }
        out[ev.sessionId + "/" + ev.task] = std::move(call);
    }
    return out;
}

// ---------------------------------------------------------------- the plans

// Which session wrote a plan.
//
// A plan file carries no session id, so the join is its write time against the
// ExitPlanMode events in the log: that tool is what writes the file, and the
// two happen within the same moment. The window is generous because the file's
// timestamp and the hook's clock are taken independently, and tight enough that
// two sessions leaving plan mode minutes apart cannot be confused.
//
// A plan matching nothing gets no session and is dropped by the caller. That is
// the common case for a directory that keeps every plan ever made.
constexpr std::int64_t kPlanJoinMs = 30 * 1000;

std::string sessionForPlan(const PlanDoc& plan, const std::vector<AgentEvent>& events)
{
    std::string  best;
    std::int64_t bestGap = kPlanJoinMs + 1;

    for (const AgentEvent& ev : events) {
        if (ev.event != "PreToolUse" || ev.tool != "ExitPlanMode" || ev.sessionId.empty())
            continue;

        const std::int64_t gap =
            ev.tsMs > plan.writtenAtMs ? ev.tsMs - plan.writtenAtMs : plan.writtenAtMs - ev.tsMs;
        if (gap <= kPlanJoinMs && gap < bestGap) {
            bestGap = gap;
            best    = ev.sessionId;
        }
    }
    return best;
}

// Where the two sources meet.
//
// `dead` is a session whose process is gone. Nothing it started is still
// running, whatever the last event said, and the same holds one level down: a
// run that has ended is proof that every agent it spawned has ended, whatever
// its journal got as far as recording.
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
            if (agent.state == AgentState::Finished)
                ++run->done;
            if (agent.failed)
                ++run->failed;
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

std::string agentLabel(const SubAgent& a)
{
    // Inside a workflow every agent is typed "workflow-subagent", which says
    // nothing the run it hangs off has not already said. The label the script
    // gave it is the part that differs between them, and the run's journal is
    // where that comes from.
    std::string label = a.workflowRun.empty() ? a.type : a.description;
    if (label.empty() && !a.workflowRun.empty())
        label = a.phase;
    if (label.empty())
        label = a.type.empty() ? "agent" : a.type;
    return label;
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
            e.task      = stringField(&doc, "task");
            e.ttlMs     = intField(&doc, "ttl");

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

// The journal is the one file here that is read again while it is still being
// written, so the cache is keyed on its size rather than on its existence. A
// run that has not started or finished an agent since the last refresh has not
// touched it, and that is the common case: the file is reparsed when it grows
// and not otherwise.
const std::vector<SubAgent>& SessionFileReader::journalFor(const fs::path&    runDir,
                                                           const std::string& runId)
{
    static const std::vector<SubAgent> kNone;

    const fs::path file = runDir / "journal.jsonl";

    // A run whose directory exists before its journal does, and every run from
    // before the journal existed at all.
    std::error_code      ec;
    const std::uintmax_t size = fs::file_size(file, ec);
    if (ec)
        return kNone;

    Journal& j = journals_[runId];
    if (j.bytes == size)
        return j.agents;

    j.agents = readJournal(file, runId);
    j.bytes  = size;
    return j.agents;
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
    if (journals_.size() > kMaxCachedRuns)
        journals_.clear();

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

    // What the run's journal says about an agent, onto what its meta file said.
    // The two are written by different halves of the same run and neither is
    // complete: the meta file has the agent's type and spawn time and cannot
    // say it ended, the journal has its label, its phase and its ending and
    // does not always name it first. An agent in one and not the other is
    // ordinary, so either may be the first to mention one.
    const auto merge = [&out](const std::vector<SubAgent>& journal) {
        for (const SubAgent& j : journal) {
            const auto at = std::find_if(out.agents.begin(), out.agents.end(),
                                         [&j](const SubAgent& a) { return a.id == j.id; });
            if (at == out.agents.end()) {
                out.agents.push_back(j);
                continue;
            }

            promote(&at->state, j.state);
            at->failed = at->failed || j.failed;
            if (!j.description.empty())
                at->description = j.description;   // the script's own label for it
            if (at->phase.empty())
                at->phase = j.phase;
        }
    };

    collect(subagents, {});

    std::error_code ec;
    const fs::path  runs = subagents / "workflows";
    if (fs::is_directory(runs, ec)) {
        for (const auto& entry : fs::directory_iterator(runs, ec)) {
            if (ec)
                break;
            if (!entry.is_directory(ec))
                continue;

            const std::string runId = narrow(entry.path().filename().wstring());
            collect(entry.path(), runId);
            merge(journalFor(entry.path(), runId));
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

AgentList buildBoard(const std::vector<RegistryEntry>&  registry,
                     const std::vector<AgentEvent>&     events,
                     const std::vector<SessionFiles>&   files,
                     const std::vector<BackgroundJob>&  jobs,
                     const std::vector<PlanDoc>&        plans,
                     const ProjectList&                 projects)
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

    // Then the background jobs, which are the other half of "a process exists".
    // A finished one is dropped here rather than shown in Done: its directory
    // is kept for ever, so admitting them would fill the board with every
    // headless session this machine has ever run.
    for (const BackgroundJob& job : jobs) {
        if (!job.live)
            continue;

        const std::string key =
            job.sessionId.empty() ? ("job:" + job.shortId) : job.sessionId;

        const auto [it, inserted] = byKey.try_emplace(key, build.size());
        if (inserted) {
            build.emplace_back();
            build.back().card.sessionId = job.sessionId;
        }
        applyJob(build[it->second], job);
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

    // Name the tasks the files found. The files know a task exists and what it
    // is saying; only the log knows what opened it, and only when hooks are
    // installed. An unnamed task still draws.
    if (const auto calls = taskCalls(events); !calls.empty()) {
        for (Building& b : build) {
            for (BackgroundTask& t : b.card.tasks) {
                const auto at = calls.find(b.card.sessionId + "/" + t.id);
                if (at == calls.end())
                    continue;
                t.kind        = at->second.kind;
                t.label       = at->second.label;
                t.agentId     = at->second.agentId;
                t.expiresAtMs = at->second.expiresAtMs;
            }
        }
    }

    // And hang each plan off whichever session wrote it. Newest first already,
    // so the first match is the one a card keeps.
    for (const PlanDoc& plan : plans) {
        const std::string owner = sessionForPlan(plan, events);
        if (owner.empty())
            continue;

        const auto it = byKey.find(owner);
        if (it == byKey.end())
            continue;

        AgentCard& c = build[it->second].card;
        if (!c.plans.empty())
            continue;   // a session's current plan, not its history

        PlanDoc joined = plan;
        joined.sessionId = owner;
        c.plans.push_back(std::move(joined));
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
    const fs::path local = cf::localAppData();
    if (local.empty())
        return {};
    return local / "ProjectMan" / "agents" / "events.jsonl";
}

fs::path sessionRegistryDir()
{
    const fs::path profile = cf::userProfile();
    if (profile.empty())
        return {};
    return profile / ".claude" / "sessions";
}

fs::path sessionDataRoot()
{
    const fs::path profile = cf::userProfile();
    if (profile.empty())
        return {};
    return profile / ".claude" / "projects";
}

} // namespace pm::gui
