#include "claude_state.h"

#include "claude_files.h"
#include "json.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <system_error>

namespace pm::gui {
namespace {

using cf::anyString;
using cf::claudeHome;
using cf::fileWriteTimeMs;
using cf::intField;
using cf::iso8601Ms;
using cf::processStillOurs;
using cf::readShared;
using cf::readSharedHead;
using cf::stringField;
using cf::ticksField;
using cf::writerHoldsOpen;

// A task's output file. Only its tail is wanted, and a build log or a `tail -f`
// left running reaches megabytes: readShared keeps the end, which is the part
// that says what the task is doing now.
constexpr std::int64_t kMaxTaskBytes = 64 * 1024;

// One task item is a couple of hundred bytes.
constexpr std::int64_t kMaxTodoBytes = 256 * 1024;

// A job's state file carries its whole prompt and its last result. The largest
// here is 4KB; this is a bound on what will be parsed rather than a size
// anything is expected to reach.
constexpr std::int64_t kMaxJobBytes = 1024 * 1024;

// A job timeline grows by a line per state change, each carrying the whole text
// the job emitted at that point. Only the last line is wanted.
constexpr std::int64_t kMaxTimelineBytes = 256 * 1024;

// Only the leading heading is read out of a plan. The body can be tens of
// kilobytes and none of it is drawn.
constexpr std::int64_t kMaxPlanHeadBytes = 8 * 1024;

// What a node can show of a task's output, and of the command that started it.
// Past this the text is cut with an ellipsis rather than wrapped.
constexpr std::size_t kTailChars = 160;

// A session with more tasks in flight than this is not one a graph can say
// anything useful about, and the directory is not cleaned up while the session
// lives. Live ones are kept first, so the cut falls on finished tasks.
constexpr std::size_t kMaxTasks = 64;

// The whole task list of a session. Claude Code numbers these from one and
// nothing here has seen past a few dozen.
constexpr std::size_t kMaxTodos = 512;

// Job directories accumulate for ever, one per background session ever started.
constexpr std::size_t kMaxJobs = 256;

// A task file nobody holds open and nothing has written to in this long is
// finished, whatever kind it is. Monitor caps its own watch at an hour, so an
// hour plus a margin cannot cut off one that is still armed, and it keeps the
// per-refresh work to the handful of files that are recent rather than the
// whole history of a session that has been open since yesterday.
constexpr std::int64_t kStaleMs = 70 * 60 * 1000;

// ---------------------------------------------------------------- utilities

bool isDirectory(const fs::path& p)
{
    std::error_code ec;
    return !p.empty() && fs::is_directory(p, ec);
}

// Cut to a character budget, on a character rather than mid-escape, with an
// ellipsis when anything was dropped.
std::string clip(std::string text, std::size_t budget)
{
    if (text.size() <= budget)
        return text;
    text.resize(budget);
    // Do not end on the lead byte of a UTF-8 sequence whose tail was cut off.
    while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xC0) == 0x80)
        text.pop_back();
    if (!text.empty())
        text.pop_back();
    text += "...";
    return text;
}

// The last line of a block of output with anything on it.
//
// A tool that redraws a progress bar ends its file with a carriage return and
// no newline, and a shell that has just started may have written nothing at
// all, so this walks back rather than taking whatever follows the final '\n'.
std::string lastLine(const std::string& text)
{
    std::size_t end = text.size();
    while (end > 0) {
        // Skip the run of separators this line ends with.
        while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r'))
            --end;
        if (end == 0)
            break;

        std::size_t start = end;
        while (start > 0 && text[start - 1] != '\n' && text[start - 1] != '\r')
            --start;

        const std::string_view line =
            pm::trim(std::string_view(text).substr(start, end - start));
        if (!line.empty())
            return std::string(line);
        end = start;
    }
    return {};
}

// A JSON array of strings, skipping anything in it that is not one.
std::vector<std::string> stringArray(const json::Value* object, const char* key)
{
    std::vector<std::string> out;
    if (!object)
        return out;
    const json::Value* v = object->find(key);
    if (!v || v->type != json::Value::Type::Array)
        return out;
    for (const json::Value& item : v->array) {
        if (item.type == json::Value::Type::String && !item.string.empty())
            out.push_back(item.string);
    }
    return out;
}

// Parse a whole file as one JSON object. Null on anything that is not one,
// which covers a file caught half-written as well as a shape that changed.
bool parseObject(const fs::path& file, std::int64_t maxBytes, json::Value* out)
{
    const std::string text = readShared(file, maxBytes);
    if (text.empty())
        return false;
    std::string error;
    if (!json::parse(text, out, &error))
        return false;
    return out->type == json::Value::Type::Object;
}

} // namespace

const char* taskKindLabel(TaskKind k)
{
    switch (k) {
    case TaskKind::Shell:   return "shell";
    case TaskKind::Monitor: return "monitor";
    case TaskKind::Unknown: break;
    }
    return "task";
}

// ----------------------------------------------------------------- locations

fs::path claudeTempRoot()
{
    wchar_t      buffer[MAX_PATH + 1]{};
    const DWORD  n = GetTempPathW(MAX_PATH, buffer);
    if (n == 0 || n > MAX_PATH)
        return {};
    return fs::path(buffer) / "claude";
}

fs::path todoRoot()
{
    const fs::path home = claudeHome();
    return home.empty() ? fs::path() : home / "tasks";
}

fs::path jobRoot()
{
    const fs::path home = claudeHome();
    return home.empty() ? fs::path() : home / "jobs";
}

fs::path planRoot()
{
    const fs::path home = claudeHome();
    return home.empty() ? fs::path() : home / "plans";
}

// ------------------------------------------------------------ shells and monitors

const fs::path& TaskReader::directoryFor(const std::string& sessionId, const fs::path& cwd)
{
    if (const auto at = dirs_.find(sessionId); at != dirs_.end())
        return at->second;

    fs::path       found;
    const fs::path root = claudeTempRoot();

    if (!root.empty()) {
        // The derivation, which is right for every directory on this machine.
        if (!cwd.empty()) {
            const fs::path guess = root / cf::projectSlug(cwd) / sessionId / "tasks";
            if (isDirectory(guess))
                found = guess;
        }

        // The fallback, for a slug encoded some way this does not know about.
        // One pass over the slug level, testing a path rather than reading a
        // directory, so a temp root holding a hundred projects costs a hundred
        // attribute lookups once per session and never again.
        if (found.empty()) {
            std::error_code ec;
            for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
                if (ec)
                    break;
                if (!e.is_directory(ec))
                    continue;
                const fs::path candidate = e.path() / sessionId / "tasks";
                if (isDirectory(candidate)) {
                    found = candidate;
                    break;
                }
            }
        }
    }

    return dirs_.emplace(sessionId, std::move(found)).first->second;
}

std::vector<BackgroundTask> TaskReader::read(const std::string& sessionId, const fs::path& cwd,
                                            std::int64_t nowMs)
{
    std::vector<BackgroundTask> out;
    if (sessionId.empty())
        return out;

    const fs::path dir = directoryFor(sessionId, cwd);
    if (dir.empty())
        return out;

    // A session that had no tasks when it was first read gets one later, so a
    // cached empty path is re-derived the moment the directory appears. The
    // derivation is a string operation and a stat; only the scan is expensive,
    // and this never reaches it.
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;

    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".output")
            continue;

        BackgroundTask t;
        t.id = e.path().stem().string();
        if (t.id.empty())
            continue;

        // The cheap tests first, and the file itself only for what passes them.
        //
        // Nothing here ever cleans this directory up. A session open since
        // yesterday leaves sixty files in it, one of which was 1.1GB when this
        // was written, and reading all of them every five seconds to describe
        // work that finished last night is the cost this avoids.
        //
        // Two tests, because neither alone covers both kinds. The handle is
        // exact for a shell and always false for a monitor; the write time is
        // the only thing left that can speak for a monitor, and a monitor that
        // has been silent longer than its own maximum watch cannot be armed
        // whatever it declared. Nothing is decided here: BackgroundTask::live
        // decides, once the join has said which kind this is.
        t.writing      = writerHoldsOpen(e.path());
        t.lastOutputMs = fileWriteTimeMs(e.path());

        if (!t.writing && (nowMs - t.lastOutputMs) > kStaleMs)
            continue;

        t.tail = clip(lastLine(readShared(e.path(), kMaxTaskBytes)), kTailChars);

        out.push_back(std::move(t));
    }

    // Whatever spoke most recently, so the cut below falls on the quiet ones.
    std::sort(out.begin(), out.end(), [](const BackgroundTask& a, const BackgroundTask& b) {
        return a.lastOutputMs > b.lastOutputMs;
    });
    if (out.size() > kMaxTasks)
        out.resize(kMaxTasks);
    return out;
}

// ------------------------------------------------------------------- task list

std::vector<TodoItem> readTodos(const std::string& sessionId)
{
    std::vector<TodoItem> out;
    if (sessionId.empty())
        return out;

    const fs::path root = todoRoot();
    if (root.empty())
        return out;

    const fs::path dir = root / sessionId;
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;

    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (ec)
            break;
        // .lock and .highwatermark sit alongside the items and are not items.
        if (!e.is_regular_file(ec) || e.path().extension() != ".json")
            continue;

        json::Value doc;
        if (!parseObject(e.path(), kMaxTodoBytes, &doc))
            continue;

        TodoItem item;
        // The file's own name is what blocks and blockedBy name, so it wins
        // over an id inside that disagrees with it.
        item.id         = e.path().stem().string();
        item.subject    = anyString(&doc, "subject");
        item.activeForm = anyString(&doc, "activeForm");
        item.status     = anyString(&doc, "status");
        item.blocks     = stringArray(&doc, "blocks");
        item.blockedBy  = stringArray(&doc, "blockedBy");

        if (item.subject.empty() && item.activeForm.empty())
            continue;

        out.push_back(std::move(item));
        if (out.size() >= kMaxTodos)
            break;
    }

    // Numeric where the names are numbers, which is every one seen, and
    // lexicographic otherwise rather than throwing the list away.
    std::sort(out.begin(), out.end(), [](const TodoItem& a, const TodoItem& b) {
        char* ea = nullptr;
        char* eb = nullptr;
        const long na = std::strtol(a.id.c_str(), &ea, 10);
        const long nb = std::strtol(b.id.c_str(), &eb, 10);
        const bool bothNumeric = ea && *ea == '\0' && eb && *eb == '\0';
        return bothNumeric ? na < nb : a.id < b.id;
    });
    return out;
}

// ------------------------------------------------------ background Claude sessions

namespace {

// The daemon's roster, as short id to the pid running it. This is what makes a
// job live: a state file says what the job last reported, and a job killed
// without warning never gets to report anything.
std::unordered_map<std::string, unsigned long> rosterPids()
{
    std::unordered_map<std::string, unsigned long> out;

    const fs::path home = claudeHome();
    if (home.empty())
        return out;

    json::Value doc;
    if (!parseObject(home / "daemon" / "roster.json", kMaxJobBytes, &doc))
        return out;

    const json::Value* workers = doc.find("workers");
    if (!workers || workers->type != json::Value::Type::Object)
        return out;

    for (const auto& [shortId, entry] : workers->object) {
        if (entry.type != json::Value::Type::Object)
            continue;
        const auto pid   = static_cast<unsigned long>(intField(&entry, "pid"));
        const auto ticks = ticksField(&entry, "procStart");
        const auto since = intField(&entry, "startedAt");
        if (pid != 0 && processStillOurs(pid, ticks, since))
            out.emplace(shortId, pid);
    }
    return out;
}

// The most recent line of a job's timeline, which carries its current state and
// the one-line detail the state file may not have caught up to yet.
void applyTimeline(const fs::path& dir, BackgroundJob* job)
{
    const std::string text = readShared(dir / "timeline.jsonl", kMaxTimelineBytes);
    if (text.empty())
        return;

    // Walk back through the lines rather than parsing all of them: only the
    // last well-formed record matters and each carries the job's whole output
    // at that point.
    std::size_t end = text.size();
    while (end > 0) {
        while (end > 0 && (text[end - 1] == '\n' || text[end - 1] == '\r'))
            --end;
        if (end == 0)
            return;

        std::size_t start = end;
        while (start > 0 && text[start - 1] != '\n')
            --start;

        json::Value doc;
        std::string error;
        if (json::parse(std::string_view(text).substr(start, end - start), &doc, &error)
            && doc.type == json::Value::Type::Object) {
            const std::string state  = anyString(&doc, "state");
            const std::string detail = anyString(&doc, "detail");
            if (!state.empty())
                job->state = state;
            if (!detail.empty())
                job->detail = clip(detail, kTailChars);
            if (const std::int64_t at = iso8601Ms(stringField(&doc, "at")); at > job->updatedAtMs)
                job->updatedAtMs = at;
            return;
        }
        end = start;
    }
}

} // namespace

std::vector<BackgroundJob> readJobs()
{
    std::vector<BackgroundJob> out;

    const fs::path root = jobRoot();
    if (root.empty())
        return out;

    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return out;

    const std::unordered_map<std::string, unsigned long> live = rosterPids();

    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!e.is_directory(ec))
            continue;

        json::Value doc;
        if (!parseObject(e.path() / "state.json", kMaxJobBytes, &doc))
            continue;

        BackgroundJob job;
        job.shortId   = e.path().filename().string();
        job.sessionId = stringField(&doc, "sessionId");
        job.name      = anyString(&doc, "name");
        job.intent    = clip(anyString(&doc, "intent"), kTailChars);
        job.state     = anyString(&doc, "state");
        job.detail    = clip(anyString(&doc, "detail"), kTailChars);
        job.tokens    = intField(&doc, "tokens");

        if (const std::string cwd = stringField(&doc, "cwd"); !cwd.empty())
            job.cwd = fs::path(cwd);

        job.createdAtMs = iso8601Ms(stringField(&doc, "createdAt"));
        job.updatedAtMs = iso8601Ms(stringField(&doc, "updatedAt"));

        // The roster decides, and a job it does not name is finished whatever
        // its own state file says. A supervisor that died takes the roster with
        // it, which reads as every job finished: right, since the workers went
        // with it.
        if (const auto at = live.find(job.shortId); at != live.end()) {
            job.pid  = at->second;
            job.live = true;
        }

        applyTimeline(e.path(), &job);

        if (job.name.empty())
            job.name = job.shortId;

        out.push_back(std::move(job));
        if (out.size() >= kMaxJobs)
            break;
    }

    std::sort(out.begin(), out.end(), [](const BackgroundJob& a, const BackgroundJob& b) {
        if (a.live != b.live)
            return a.live;
        return a.updatedAtMs > b.updatedAtMs;
    });
    return out;
}

// -------------------------------------------------------------------- plans

std::vector<PlanDoc> readPlans(std::int64_t withinMs)
{
    std::vector<PlanDoc> out;

    const fs::path root = planRoot();
    if (root.empty())
        return out;

    std::error_code ec;
    if (!fs::is_directory(root, ec))
        return out;

    const std::int64_t now   = cf::ticksToUnixMs([] {
        FILETIME       ft{};
        ULARGE_INTEGER t{};
        GetSystemTimeAsFileTime(&ft);
        t.LowPart  = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        return t.QuadPart;
    }());
    const std::int64_t floor = (withinMs > 0 && now > withinMs) ? now - withinMs : 0;

    for (const fs::directory_entry& e : fs::directory_iterator(root, ec)) {
        if (ec)
            break;
        if (!e.is_regular_file(ec) || e.path().extension() != ".md")
            continue;

        const std::int64_t written = fileWriteTimeMs(e.path());
        if (written < floor)
            continue;

        PlanDoc plan;
        plan.file        = e.path();
        plan.writtenAtMs = written;

        // The leading "# " heading, which is what ExitPlanMode writes first.
        // Falls back to the file's own name, which is a slug of the prompt.
        //
        // readSharedHead and not readShared: a plan is a document, and the one
        // line wanted from it is its first. readShared keeps the END of a file
        // past its budget, which is right for every log here and silently wrong
        // for this, returning the tail of a long plan and no heading at all.
        const std::string head = readSharedHead(e.path(), kMaxPlanHeadBytes);
        for (const std::string_view line : pm::splitLines(head)) {
            const std::string_view t = pm::trim(line);
            if (t.rfind("# ", 0) == 0) {
                plan.title = clip(std::string(pm::trim(t.substr(2))), kTailChars);
                break;
            }
            if (!t.empty())
                break;   // body before any heading: there is no title to find
        }
        if (plan.title.empty())
            plan.title = e.path().stem().string();

        out.push_back(std::move(plan));
    }

    std::sort(out.begin(), out.end(), [](const PlanDoc& a, const PlanDoc& b) {
        return a.writtenAtMs > b.writtenAtMs;
    });
    return out;
}

} // namespace pm::gui
