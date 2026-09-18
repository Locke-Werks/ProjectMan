#include "hook.h"

#include "claude_stream.h"
#include "console.h"
#include "json.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace pm::cli {
namespace {

// Exit codes. main.cpp keeps its own copies in its own anonymous namespace and
// the two agree; sharing them would mean a header for three integers.
constexpr int kOk    = 0;
constexpr int kError = 1;
constexpr int kUsage = 2;

int fail(const std::string& msg)
{
    std::fprintf(stderr, "projectman: %s\n", msg.c_str());
    return kError;
}

// For the two verbs that were about to write. Saying the file is untouched is
// the part that matters: someone whose settings.json has a stray comma in it
// needs to know the damage was not compounded.
int refuse(const std::string& msg)
{
    std::fprintf(stderr, "projectman: %s\n", msg.c_str());
    std::fprintf(stderr, "projectman: the file was not changed\n");
    return kError;
}

int failUsage()
{
    std::fprintf(stderr, "projectman: usage: pm hook [install|uninstall|status]\n");
    std::fprintf(stderr, "projectman: with no argument it reads a hook payload on stdin\n");
    return kUsage;
}

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

// The value of kTestRootVar, empty when it is unset or empty. See hook.h: the
// only reason this exists is that every other way of exercising install and
// uninstall writes to the real ~/.claude/settings.json.
fs::path testRoot()
{
    const DWORD n = GetEnvironmentVariableW(kTestRootVar, nullptr, 0);
    if (n == 0)
        return {};
    std::wstring buf(n, L'\0');
    const DWORD  got = GetEnvironmentVariableW(kTestRootVar, buf.data(), n);
    if (got == 0 || got >= n)
        return {};
    buf.resize(got);
    return fs::path(buf);
}

fs::path selfPath()
{
    wchar_t     buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return {};
    return fs::path(std::wstring(buf, n));
}

// ------------------------------------------------------------- the receiver

// A hook payload is normally a few hundred bytes, but a Write or an Edit
// carries the whole file in tool_input. The cap bounds a process that runs on
// every tool call in every session; past it the record is dropped, which costs
// one card's detail rather than an allocation nobody asked for.
constexpr size_t kMaxPayloadBytes = 32u * 1024u * 1024u;

// What a card can show of a prompt. The same figure claude_stream.cpp cuts a
// tool summary to, so a line is a line wherever it came from.
constexpr size_t kDetailChars = 120;

std::string stringField(const json::Value* object, const char* key)
{
    if (!object)
        return {};
    const json::Value* v = object->find(key);
    return (v && v->type == json::Value::Type::String) ? v->string : std::string();
}

// First line only, cut to fit, with a marker for what was cut.
//
// A copy of claude_stream.cpp's, which is file-local there, plus one thing it
// does not do: the cut backs off to a UTF-8 lead byte. A prompt is typed by a
// person and can carry anything, and half a codepoint written into the log
// reaches the board as a replacement character.
std::string oneLine(std::string s, size_t max)
{
    const size_t nl  = s.find_first_of("\r\n");
    bool         cut = false;
    if (nl != std::string::npos) {
        s.resize(nl);
        cut = true;
    }
    if (s.size() > max) {
        size_t end = max;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80)
            --end;
        s.resize(end);
        cut = true;
    }
    if (cut)
        s += " ...";
    return s;
}

// The marker oneLine leaves behind, here and in claude_stream.cpp.
constexpr std::string_view kCutMarker = " ...";

// Drops a trailing UTF-8 sequence that is missing its remaining bytes. A cut
// made by byte count leaves one behind whenever the character it landed in was
// multibyte.
void dropPartialCodepoint(std::string& s)
{
    size_t lead = s.size();
    while (lead > 0 && (static_cast<unsigned char>(s[lead - 1]) & 0xC0) == 0x80)
        --lead;
    if (lead == 0)
        return;
    --lead;

    const unsigned char b = static_cast<unsigned char>(s[lead]);
    size_t              need = 1;
    if ((b & 0xF8) == 0xF0)
        need = 4;
    else if ((b & 0xF0) == 0xE0)
        need = 3;
    else if ((b & 0xE0) == 0xC0)
        need = 2;

    if (lead + need > s.size())
        s.resize(lead);
}

// A tool summary, cut the way this file cuts everything else.
//
// claude_stream's summary is already cut, by a bare resize with no lead-byte
// backoff, and PreToolUse is the most frequent event there is: running it back
// through the local oneLine is what puts that protection on the common case.
// The marker comes off first because the split character is underneath it, and
// goes back on afterwards so a cut still reads as one.
std::string recutSummary(std::string s)
{
    const bool marked = s.ends_with(kCutMarker);
    if (marked)
        s.resize(s.size() - kCutMarker.size());

    dropPartialCodepoint(s);

    std::string out = oneLine(std::move(s), kDetailChars);
    if (marked && !out.ends_with(kCutMarker))
        out += kCutMarker;
    return out;
}

// Everything on stdin, as bytes.
//
// ReadFile on the handle rather than a CRT text-mode read: the payload is UTF-8
// JSON, and text mode would rewrite every CRLF inside it, changing what a
// prompt or a Bash command actually said.
std::string readStdin()
{
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE)
        return {};

    std::string body;
    char        buf[16384];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;
        // Draining continues past the cap. Leaving bytes in the pipe fails the
        // write on the far end, and the session doing the writing is not ours
        // to break over a log line.
        if (body.size() < kMaxPayloadBytes)
            body.append(buf, got);
    }
    return body;
}

std::int64_t nowUnixMs()
{
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER ticks{};
    ticks.LowPart  = ft.dwLowDateTime;
    ticks.HighPart = ft.dwHighDateTime;

    // FILETIME counts 100ns intervals from 1601-01-01 UTC, 11644473600 seconds
    // before the Unix epoch.
    constexpr std::int64_t kEpochOffset = 11644473600LL * 10000000LL;
    return (static_cast<std::int64_t>(ticks.QuadPart) - kEpochOffset) / 10000;
}

// Which half of an append failed. The caller can retry an open; it can never
// retry a write, because a short write has already put bytes on the disk and
// sending the line again would glue a fragment to a complete record.
enum class Append { Ok, OpenFailed, WriteFailed };

// One line, appended whole.
//
// FILE_APPEND_DATA rather than GENERIC_WRITE, and one WriteFile rather than a
// stream: that pair is the only combination Windows guarantees lands at the end
// of the file intact. Several sessions hold this file open at once, and a
// seek-then-write would interleave two half lines.
//
// FILE_SHARE_DELETE is in the share mode so a rotation running in another
// session can rename the log out from under this handle instead of failing.
// The handle follows the file, so bytes written through it are not lost.
Append appendLine(const fs::path& log, const std::string& line)
{
    const HANDLE h = CreateFileW(log.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return Append::OpenFailed;

    DWORD      written = 0;
    const BOOL ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()),
                              &written, nullptr);
    CloseHandle(h);
    if (ok == 0 || static_cast<size_t>(written) != line.size())
        return Append::WriteFailed;
    return Append::Ok;
}

void rotateIfFull(const fs::path& log)
{
    // The size test and the rename have to name the same file, not the same
    // path. Two sessions that both measured a full log by path would have the
    // second one rename the fresh log the first had just created, and
    // MOVEFILE_REPLACE_EXISTING would put it over the back-file holding all the
    // history. One handle, opened before the size is read and renamed by that
    // same handle, keeps the loser pointed at the file it actually measured:
    // its rename lands on a file that is already the back-file and changes
    // nothing.
    const HANDLE h = CreateFileW(log.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < kMaxLogBytes) {
        CloseHandle(h);
        return;
    }

    // One back-file, and a rotation nobody waits on. A session holding the log
    // without FILE_SHARE_DELETE fails this rename, and that is the right
    // outcome: a skipped rotation costs disk, a dropped event costs a card its
    // face.
    const std::wstring target = (log.parent_path() / "events.1.jsonl").wstring();

    // FILE_RENAME_INFO carries the name past the end of the struct, so the
    // struct has to be placed in a buffer big enough for both.
    const size_t nameBytes = (target.size() + 1) * sizeof(wchar_t);
    const size_t bytes     = sizeof(FILE_RENAME_INFO) + nameBytes;

    std::vector<unsigned char> buf(bytes, 0);

    auto* info            = reinterpret_cast<FILE_RENAME_INFO*>(buf.data());
    info->ReplaceIfExists = static_cast<BOOLEAN>(TRUE);
    info->RootDirectory   = nullptr;
    info->FileNameLength  = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    std::memcpy(info->FileName, target.c_str(), nameBytes);

    SetFileInformationByHandle(h, FileRenameInfo, info, static_cast<DWORD>(bytes));
    CloseHandle(h);
}

int receiveBody()
{
    // Nothing is on stdin when a person types `pm hook` at a prompt, and the
    // read would sit there until they found Ctrl+Z. A console is proof this is
    // not Claude Code calling, so it is also the one case where the receiver
    // may speak.
    //
    // It still exits 0. Claude Code reads 2 from a PreToolUse hook as "block
    // this tool call" and hands the hook's stderr to the model, so a receiver
    // that found a console on a handle it did not expect would stop the user's
    // work and paste usage text into their session.
    if (stdinIsConsole()) {
        failUsage();
        return kOk;
    }

    const std::string body = readStdin();

    json::Value payload;
    std::string parseError;
    if (!json::parse(body, &payload, &parseError))
        return kOk;

    const std::string event = stringField(&payload, "hook_event_name");
    if (event.empty())
        return kOk;   // nothing the board could file it under

    const std::string tool =
        event == "PreToolUse" ? stringField(&payload, "tool_name") : std::string();

    // Every branch goes through the same cap. A Notification message has no
    // length limit of its own, and one long enough would write a log line of
    // megabytes, spending the whole rotation budget on a single event and
    // throwing away every session's history a rotation early.
    std::string detail;
    if (event == "SessionStart")
        detail = oneLine(stringField(&payload, "source"), kDetailChars);
    else if (event == "UserPromptSubmit")
        detail = oneLine(stringField(&payload, "prompt"), kDetailChars);
    else if (event == "PreToolUse")
        detail = recutSummary(claude::toolSummary(tool, payload.find("tool_input")));
    else if (event == "Notification")
        detail = oneLine(stringField(&payload, "message"), kDetailChars);
    else if (event == "SessionEnd")
        detail = oneLine(stringField(&payload, "reason"), kDetailChars);
    // Stop carries nothing of its own: the card keeps whatever came before it.

    std::vector<std::pair<std::string, json::Value>> record;
    record.reserve(6);
    record.emplace_back("ts", json::makeInt(nowUnixMs()));
    record.emplace_back("event", json::makeString(event));

    const std::string session = stringField(&payload, "session_id");
    if (!session.empty())
        record.emplace_back("session", json::makeString(session));

    const std::string cwd = stringField(&payload, "cwd");
    if (!cwd.empty())
        record.emplace_back("cwd", json::makeString(cwd));

    if (!tool.empty())
        record.emplace_back("tool", json::makeString(tool));
    if (!detail.empty())
        record.emplace_back("detail", json::makeString(detail));

    const fs::path log = eventLogPath();
    if (log.empty())
        return kOk;

    const std::string line = json::dump(json::makeObject(std::move(record))) + "\n";

    rotateIfFull(log);

    // Only a failed open is retried. The directory is missing exactly once per
    // machine, and creating it after a failed open costs nothing on the
    // thousands of events that follow, where create_directories would be a
    // syscall every time. A short write is not retried at any price: those
    // bytes are on the disk, and a second copy of the whole line behind them
    // would leave one line no reader can parse.
    if (appendLine(log, line) == Append::OpenFailed) {
        std::error_code ec;
        fs::create_directories(log.parent_path(), ec);
        (void)appendLine(log, line);
    }

    return kOk;
}

int receive()
{
    // The contract is exit 0 and say nothing, and until now nothing but the
    // absence of a throw was holding it. readStdin grows to 32MB by doubling,
    // the parse builds a tree on top of that and every string is rebuilt again,
    // so a big payload in several sessions at once can put a bad_alloc through
    // main, where terminate prints the CRT's "requested the Runtime to
    // terminate it in an unusual way" and raises WER in the middle of a turn.
    // The two calls cover the paths that do not go through an exception: a
    // failing drive and an abort out of the CRT itself.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);

    try {
        return receiveBody();
    } catch (...) {
        return kOk;
    }
}

// ------------------------------------------------------------- registration

// Every event install registers. PostToolUse is deliberately absent: a tool
// that has already returned is the previous state of the card, bought by
// doubling the write rate on the busiest event there is.
constexpr const char* kEvents[] = {
    "SessionStart",
    "UserPromptSubmit",
    "PreToolUse",
    "Notification",
    "Stop",
    "SessionEnd",
};

// Seconds. Long enough for a cold open of a file on a slow disk, short enough
// that a machine where the log has become unwritable does not hold up a turn.
constexpr std::int64_t kTimeoutSeconds = 5;

// The tag install writes into every entry it adds, and the first thing
// uninstall matches on. Claude Code ignores keys it does not know, which makes
// this surer than matching the command text: the path moves when pm does, and
// somebody else's hook is entitled to run pm for reasons of its own.
constexpr const char* kMarkerKey = "_projectman";

fs::path settingsPath()
{
    if (const fs::path root = testRoot(); !root.empty())
        return root / "settings.json";

    const fs::path profile = knownFolder(FOLDERID_Profile);
    if (profile.empty())
        return {};
    return profile / ".claude" / "settings.json";
}

// The command Claude Code is asked to run, quoted for a SHELL and not for
// CreateProcess.
//
// The two rules are not the same and the difference is silent. proc::quoteArg
// follows the CRT's argv rule, which leaves an argument alone unless it holds a
// space, and backslashes mean nothing to CreateProcess. Claude Code runs a hook
// through bash, where an unquoted backslash is an escape character, so
// C:\Users\me\pm.exe arrived as CUsersmepm.exe and every hook on this machine
// failed with "command not found". It went unnoticed because an installed
// pm.exe lives under Program Files, whose space makes quoteArg quote it: the
// bug only appears when pm.exe sits in a path with no space in it, which is
// every development build.
//
// Double quotes rather than single, because they are quotes in cmd as well and
// there is no promise anywhere that the shell stays bash. Inside them bash
// still acts on four characters, so those are escaped and nothing else is: a
// path with none of them, which is all but a handful, comes out plainly
// readable in a file someone may well open.
std::string hookCommandLine(const fs::path& exe)
{
    const std::string path = narrow(exe.wstring());

    std::string out;
    out.reserve(path.size() + 8);
    out.push_back('"');

    const auto special = [](char c) {
        return c == '"' || c == '$' || c == '`' || c == '\\';
    };

    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '"' || c == '$' || c == '`') {
            out.push_back('\\');
        } else if (c == '\\' && i + 1 < path.size() && special(path[i + 1])) {
            // A separator sitting in front of something that is about to be
            // escaped has to be escaped too, or bash consumes this backslash
            // against the next one and the escape it was protecting is left
            // bare.
            out.push_back('\\');
        }
        out.push_back(c);
    }

    out.push_back('"');
    return out + " hook";
}

// Value::find is const-only, and rewriting the tree needs the other one.
json::Value* member(json::Value& v, std::string_view key)
{
    if (v.type != json::Value::Type::Object)
        return nullptr;
    for (auto& entry : v.object) {
        if (entry.first == key)
            return &entry.second;
    }
    return nullptr;
}

// The member under `key`, created empty when absent. Null counts as absent: it
// holds nothing, so replacing it destroys nothing. Returns nullptr when the key
// is there and holds something else, which is a file this has no business
// rewriting.
json::Value* ensureMember(json::Value& parent, const char* key, json::Value::Type want)
{
    if (parent.type != json::Value::Type::Object)
        return nullptr;

    json::Value* v = member(parent, key);
    if (!v) {
        parent.object.emplace_back(
            key, want == json::Value::Type::Array ? json::makeArray({}) : json::makeObject({}));
        return &parent.object.back().second;
    }
    if (v->isNull())
        *v = want == json::Value::Type::Array ? json::makeArray({}) : json::makeObject({});

    return v->type == want ? v : nullptr;
}

// True when a command line runs some pm.exe's hook receiver.
//
// The second signal, for an entry whose tag is gone. A person adjusting the
// timeout by hand, a format-on-save, or a merge can drop the key, and matching
// on the tag alone then leaves the entry running on every tool call while
// uninstall walks past it and status calls it absent. The next install appends
// a second copy beside it and doubles the hook processes per event.
//
// Deliberately looser than an equality test against this pm's own command line:
// the entry to catch is one that pm wrote and someone has since edited, and the
// exe may also have moved since.
bool commandRunsPmHook(std::string_view command)
{
    constexpr std::string_view kVerb = " hook";

    std::string_view c       = command;
    const auto       trimEnd = [&c] {
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t'))
            c.remove_suffix(1);
    };

    trimEnd();
    if (c.size() <= kVerb.size() || !iequals(c.substr(c.size() - kVerb.size()), kVerb))
        return false;
    c.remove_suffix(kVerb.size());

    trimEnd();
    if (c.size() >= 2 && c.front() == '"' && c.back() == '"') {
        c.remove_prefix(1);
        c.remove_suffix(1);
    }
    if (c.empty())
        return false;

    const fs::path exe(widen(c));
    return iequals(narrow(exe.filename().wstring()), "pm.exe");
}

// How an entry was recognised, because the two are not equally certain and
// status should say which it found.
enum class Match { None, Tagged, Untagged };

Match matchEntry(const json::Value& entry)
{
    // Presence is the signal, not the value. Uninstall has to find every entry
    // install has ever written, including from a build that wrote the tag as
    // something other than true.
    if (entry.find(kMarkerKey) != nullptr)
        return Match::Tagged;

    return commandRunsPmHook(stringField(&entry, "command")) ? Match::Untagged : Match::None;
}

bool isOurs(const json::Value& entry)
{
    return matchEntry(entry) != Match::None;
}

// One matcher group holding one entry. No "matcher" key: an absent matcher
// matches everything, and a board that wants every tool call wants everything.
json::Value hookGroup(const std::string& command)
{
    return json::makeObject({
        { "hooks", json::makeArray({ json::makeObject({
                       { "type", json::makeString("command") },
                       { "command", json::makeString(command) },
                       { "timeout", json::makeInt(kTimeoutSeconds) },
                       { kMarkerKey, json::makeBool(true) },
                   }) }) },
    });
}

// Removes every entry of ours under settings["hooks"], tagged or matched by its
// command, and the containers that leaves empty. Returns how many entries went.
//
// Only prunes a group or an event key it emptied itself. A matcher group
// somebody else left with no hooks in it is theirs, and tidying it would be a
// change uninstall was never asked to make.
int stripOurs(json::Value& settings)
{
    json::Value* hooks = member(settings, "hooks");
    if (!hooks || hooks->type != json::Value::Type::Object)
        return 0;

    int                 removed = 0;
    std::vector<size_t> emptied;

    for (size_t i = 0; i < hooks->object.size(); ++i) {
        json::Value& groups = hooks->object[i].second;
        if (groups.type != json::Value::Type::Array)
            continue;

        std::vector<json::Value> kept;
        kept.reserve(groups.array.size());

        for (json::Value& group : groups.array) {
            json::Value* entries = member(group, "hooks");
            if (!entries || entries->type != json::Value::Type::Array) {
                kept.push_back(std::move(group));
                continue;
            }

            const size_t before = entries->array.size();
            std::erase_if(entries->array, isOurs);
            const size_t gone = before - entries->array.size();
            removed += static_cast<int>(gone);

            if (gone > 0 && entries->array.empty())
                continue;

            kept.push_back(std::move(group));
        }

        const bool wentEmpty = kept.empty() && !groups.array.empty();
        groups.array         = std::move(kept);
        if (wentEmpty)
            emptied.push_back(i);
    }

    // By index, never by name. The parser keeps duplicate object keys and
    // member() answers with the first, so a file carrying two "SessionStart"
    // members, one holding somebody else's registration and one holding ours,
    // would lose both to an erase by name. Back to front, so the indices still
    // point at what they were taken from.
    for (size_t n = emptied.size(); n > 0; --n) {
        hooks->object.erase(hooks->object.begin()
                            + static_cast<std::ptrdiff_t>(emptied[n - 1]));
    }

    return removed;
}

// What is registered right now: the events carrying an entry of ours, and the
// distinct commands those entries run.
struct Registration {
    std::vector<std::string> events;
    std::vector<std::string> commands;
    // Entries recognised by their command with no tag on them. Install would
    // rewrite these, so status has to say they are there.
    int untagged = 0;
};

Registration findOurs(const json::Value& settings)
{
    Registration r;

    const json::Value* hooks = settings.find("hooks");
    if (!hooks || hooks->type != json::Value::Type::Object)
        return r;

    for (const auto& event : hooks->object) {
        if (event.second.type != json::Value::Type::Array)
            continue;

        bool here = false;
        for (const json::Value& group : event.second.array) {
            const json::Value* entries = group.find("hooks");
            if (!entries || entries->type != json::Value::Type::Array)
                continue;

            for (const json::Value& entry : entries->array) {
                const Match how = matchEntry(entry);
                if (how == Match::None)
                    continue;
                here = true;
                if (how == Match::Untagged)
                    ++r.untagged;

                const std::string command = stringField(&entry, "command");
                if (std::find(r.commands.begin(), r.commands.end(), command)
                    == r.commands.end()) {
                    r.commands.push_back(command);
                }
            }
        }

        if (here)
            r.events.push_back(event.first);
    }

    return r;
}

// ------------------------------------------------------------ the file itself

struct SettingsFile {
    fs::path    path;
    json::Value value;
    bool        existed = false;

    // What the file looked like when it was read. Claude Code writes this same
    // file while a session runs, so the write has to prove it is replacing the
    // bytes it started from.
    std::uintmax_t     size = 0;
    fs::file_time_type writeTime{};

    // Set when the file carries the same key twice somewhere. Non-empty means
    // the file means one thing to pm and another to Claude Code.
    std::string duplicateKey;
};

bool readWholeFile(const fs::path& path, std::string* out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    out->assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// The path of the first key that appears twice under the same object, or empty.
//
// json.cpp keeps duplicate members and find() answers with the first; Node
// keeps the last. So a file holding two "hooks" or two "SessionStart" means
// something different to pm than it does to the thing that runs the hooks, and
// rewriting it would settle that disagreement silently in pm's favour.
std::string duplicateKeyPath(const json::Value& v, const std::string& prefix)
{
    const auto below = [&prefix](const std::string& key) {
        return prefix.empty() ? key : prefix + "." + key;
    };

    if (v.type == json::Value::Type::Object) {
        for (size_t i = 0; i < v.object.size(); ++i) {
            const std::string& key = v.object[i].first;
            for (size_t j = 0; j < i; ++j) {
                if (v.object[j].first == key)
                    return below(key);
            }

            const std::string deeper = duplicateKeyPath(v.object[i].second, below(key));
            if (!deeper.empty())
                return deeper;
        }
        return {};
    }

    if (v.type == json::Value::Type::Array) {
        for (size_t i = 0; i < v.array.size(); ++i) {
            const std::string deeper =
                duplicateKeyPath(v.array[i], prefix + "[" + std::to_string(i) + "]");
            if (!deeper.empty())
                return deeper;
        }
    }

    return {};
}

bool readSettings(SettingsFile* s, std::string* error)
{
    s->path = settingsPath();
    if (s->path.empty()) {
        *error = "cannot resolve the user profile directory";
        return false;
    }

    // Absence and failure are not the same answer, and treating them as one
    // destroys the file. A denied ACL, a deny-share lock while Claude Code
    // rewrites the file, a cloud placeholder that cannot hydrate: every one of
    // those reads as "no settings yet", and the install that follows replaces
    // the real file with six hooks and nothing else, without a backup because
    // there was supposedly nothing to back up.
    std::error_code ec;
    const bool      present = fs::exists(s->path, ec);
    if (ec) {
        *error = s->path.string() + ": cannot tell whether it exists: " + ec.message();
        return false;
    }

    if (!present) {
        // No file is not damage. Claude Code writes one when it first has
        // something to put in it, and a hook registration is such a thing.
        s->value = json::makeObject({});
        return true;
    }

    // Set before the read, not after it. The write is about to replace whatever
    // is at this path, so it takes a backup whether or not the read worked.
    s->existed = true;

    s->size = fs::file_size(s->path, ec);
    if (ec) {
        *error = s->path.string() + ": cannot read its size: " + ec.message();
        return false;
    }
    s->writeTime = fs::last_write_time(s->path, ec);
    if (ec) {
        *error = s->path.string() + ": cannot read its timestamp: " + ec.message();
        return false;
    }

    std::string body;
    if (!readWholeFile(s->path, &body)) {
        *error = s->path.string()
                 + ": exists but cannot be read; it may be locked by another program,"
                   " denied by its permissions, or stored online only";
        return false;
    }

    // A hand-edited file on Windows can carry a byte order mark, which the
    // parser would report as a syntax error on character one.
    constexpr std::string_view kBom = "\xEF\xBB\xBF";
    if (body.starts_with(kBom))
        body.erase(0, kBom.size());

    if (trim(body).empty()) {
        s->value = json::makeObject({});
        return true;
    }

    std::string parseError;
    if (!json::parse(body, &s->value, &parseError)) {
        *error = s->path.string() + ": " + parseError;
        return false;
    }
    if (s->value.type != json::Value::Type::Object) {
        *error = s->path.string() + ": the top level is not a JSON object";
        return false;
    }

    s->duplicateKey = duplicateKeyPath(s->value, std::string());

    return true;
}

// The file on disk is still the one that was read.
//
// install and uninstall are a read-modify-write, and Claude Code writes this
// file from inside a running session: model, theme, verbose, editorMode and
// autoCompactEnabled are all live toggles. A rename built on a stale read puts
// the old value back, and the backup came from the same stale read, so the
// value that was lost is in neither file. Refusing costs the user one re-run.
bool stillAsRead(const SettingsFile& s, std::string* error)
{
    std::error_code ec;
    const bool      present = fs::exists(s.path, ec);
    if (ec) {
        *error = s.path.string() + ": cannot tell whether it exists: " + ec.message();
        return false;
    }

    if (!s.existed) {
        if (!present)
            return true;
        *error = s.path.string()
                 + " was created while pm was working; nothing was written, run the"
                   " command again";
        return false;
    }

    if (!present) {
        *error = s.path.string()
                 + " was removed while pm was working; nothing was written, run the"
                   " command again";
        return false;
    }

    const std::uintmax_t size = fs::file_size(s.path, ec);
    if (ec) {
        *error = s.path.string() + ": cannot read its size: " + ec.message();
        return false;
    }
    const fs::file_time_type stamp = fs::last_write_time(s.path, ec);
    if (ec) {
        *error = s.path.string() + ": cannot read its timestamp: " + ec.message();
        return false;
    }

    if (size != s.size || stamp != s.writeTime) {
        *error = s.path.string()
                 + " changed while pm was working on it; nothing was written, run the"
                   " command again";
        return false;
    }

    return true;
}

// True for a settings.json that is a symlink, or a hardlink with another name
// somewhere else.
bool isLinked(const fs::path& path)
{
    std::error_code ec;
    if (fs::is_symlink(path, ec) && !ec)
        return true;

    ec.clear();
    const std::uintmax_t links = fs::hard_link_count(path, ec);
    return !ec && links > 1;
}

bool writeThrough(const fs::path& path, const std::string& body, std::string* error)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        *error = "cannot write " + path.string();
        return false;
    }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    f.close();
    if (!f) {
        *error = "cannot write " + path.string();
        return false;
    }
    return true;
}

bool writeSettings(const SettingsFile& s, std::string* error)
{
    if (!stillAsRead(s, error))
        return false;

    if (s.existed) {
        fs::path backup = s.path;
        backup += L".pm-backup";

        std::error_code ec;
        fs::copy_file(s.path, backup, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            *error = "cannot write " + backup.string() + ": " + ec.message();
            return false;
        }
    }

    // dumpPretty, never dump. This file is read and edited by hand, and
    // collapsing it to one line would be a worse change than the one being made.
    std::string body = json::dumpPretty(s.value, 2);
    body += "\n";

    // A rename replaces the name, so a settings.json that is a symlink or a
    // hardlink into a dotfiles repo would come out of this a plain file, with
    // the repo's copy still holding the old content and nothing saying so.
    // Those are written through the path they already have, which keeps the
    // link; the backup taken above is what covers an interrupted write there.
    if (s.existed && isLinked(s.path)) {
        if (!stillAsRead(s, error))
            return false;
        return writeThrough(s.path, body, error);
    }

    // Write a sibling and rename over the target, the way Config::save does, so
    // an interrupted write cannot leave a truncated settings.json behind.
    fs::path tmp = s.path;
    tmp += L".pm-tmp";
    if (!writeThrough(tmp, body, error))
        return false;

    // Last look before the replace, so the window between the read and the
    // rename is as small as this can make it.
    if (!stillAsRead(s, error)) {
        std::error_code ec;
        fs::remove(tmp, ec);
        return false;
    }

    if (!MoveFileExW(tmp.c_str(), s.path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        *error = "cannot replace " + s.path.string() + ": " + errorText(GetLastError());
        return false;
    }

    return true;
}

// ------------------------------------------------------------------ the verbs

std::string joined(const std::vector<std::string>& items)
{
    std::string out;
    for (const std::string& item : items)
        out += (out.empty() ? "" : ", ") + item;
    return out;
}

std::vector<std::string> allEvents()
{
    return std::vector<std::string>(std::begin(kEvents), std::end(kEvents));
}

// What the two writing verbs say about a file whose duplicate keys mean one
// thing here and another to Claude Code. Rewriting it would pick pm's reading
// and leave no sign the other one ever existed.
std::string ambiguous(const SettingsFile& s)
{
    return s.path.string() + ": \"" + s.duplicateKey
           + "\" appears twice; Claude Code reads the last one and pm reads the first,"
             " so remove one of them by hand first";
}

int install()
{
    const fs::path exe = selfPath();
    if (exe.empty())
        return fail("cannot resolve this executable's own path");

    SettingsFile s;
    std::string  err;
    if (!readSettings(&s, &err))
        return refuse(err);
    if (!s.duplicateKey.empty())
        return refuse(ambiguous(s));

    // Install is also repair. Stripping first means running it twice registers
    // one set rather than two, and running it from a new location repoints the
    // old entries instead of racing them.
    const int replaced = stripOurs(s.value);

    json::Value* hooks = ensureMember(s.value, "hooks", json::Value::Type::Object);
    if (!hooks)
        return refuse(s.path.string() + ": \"hooks\" is not a JSON object");

    const std::string command = hookCommandLine(exe);

    for (const char* event : kEvents) {
        json::Value* groups = ensureMember(*hooks, event, json::Value::Type::Array);
        if (!groups) {
            return refuse(s.path.string() + ": \"hooks." + event
                          + "\" is not a JSON array");
        }
        groups->array.push_back(hookGroup(command));
    }

    if (!writeSettings(s, &err))
        return fail(err);

    std::printf("%s\n\n", s.path.string().c_str());
    std::printf("  %-10s %s\n", replaced > 0 ? "repointed" : "registered", command.c_str());
    std::printf("  %-10s %s\n", "events", joined(allEvents()).c_str());
    if (s.existed)
        std::printf("  %-10s %s.pm-backup\n", "backup", s.path.string().c_str());
    return kOk;
}

int uninstall()
{
    SettingsFile s;
    std::string  err;
    if (!readSettings(&s, &err))
        return refuse(err);
    if (!s.duplicateKey.empty())
        return refuse(ambiguous(s));

    const int removed = stripOurs(s.value);

    std::printf("%s\n\n", s.path.string().c_str());

    // Nothing to do means nothing written. Rewriting a file to produce the
    // bytes it already holds is a risk taken for no reason.
    if (removed == 0) {
        std::printf("  no ProjectMan hooks registered\n");
        return kOk;
    }

    if (!writeSettings(s, &err))
        return fail(err);

    std::printf("  removed %d hook%s\n", removed, removed == 1 ? "" : "s");
    std::printf("  %-10s %s.pm-backup\n", "backup", s.path.string().c_str());
    return kOk;
}

int status()
{
    SettingsFile s;
    std::string  err;
    if (!readSettings(&s, &err))
        return fail(err);

    const fs::path    exe  = selfPath();
    const std::string want = exe.empty() ? std::string() : hookCommandLine(exe);

    const Registration r = findOurs(s.value);

    std::printf("%s\n\n", s.path.string().c_str());

    if (!s.duplicateKey.empty()) {
        std::printf("  %-10s \"%s\" appears twice; Claude Code reads the last one and pm\n",
                    "ambiguous", s.duplicateKey.c_str());
        std::printf("  %-10s reads the first, so install and uninstall will not touch it\n", "");
    }

    if (r.events.empty()) {
        std::printf("  not installed\n\n");
        std::printf("  run `pm hook install` to register the board's hooks\n");
        return kOk;
    }

    std::vector<std::string> missing;
    for (const char* event : kEvents) {
        if (std::find(r.events.begin(), r.events.end(), event) == r.events.end())
            missing.push_back(event);
    }

    // A pm that cannot name its own path has nothing to compare against, and
    // reporting what is registered still beats calling it stale on no evidence.
    const bool current = want.empty() || (r.commands.size() == 1 && r.commands[0] == want);

    // Not "registered to a different pm.exe", which is only one of the reasons
    // this differs and was the wrong one the day the quoting changed: every
    // install on earth was suddenly out of date while still naming the same
    // executable. The two lines below say which, so the headline does not
    // guess.
    std::printf("  %s\n", current ? "installed"
                                  : "installed, but the registered command is out of date");
    for (const std::string& command : r.commands)
        std::printf("  %-10s %s\n", "runs", command.c_str());
    if (!current && !want.empty())
        std::printf("  %-10s %s\n", "should run", want.c_str());
    std::printf("  %-10s %s\n", "events", joined(r.events).c_str());
    if (!missing.empty())
        std::printf("  %-10s %s\n", "missing", joined(missing).c_str());

    // An entry matched by its command alone has been edited since install wrote
    // it. It still runs, and install will rewrite it tagged, so say it is there
    // rather than reporting a clean registration.
    if (r.untagged > 0) {
        std::printf("  %-10s %d entr%s with no %s tag, matched by command\n", "untagged",
                    r.untagged, r.untagged == 1 ? "y" : "ies", kMarkerKey);
    }

    if (!current || !missing.empty() || r.untagged > 0)
        std::printf("\n  run `pm hook install` to bring it up to date\n");

    return kOk;
}

} // namespace

int hookCommand(const std::vector<std::string>& args)
{
    if (args.empty())
        return receive();

    const std::string& sub = args[0];
    if (iequals(sub, "install"))
        return install();
    if (iequals(sub, "uninstall"))
        return uninstall();
    if (iequals(sub, "status"))
        return status();

    return failUsage();
}

fs::path eventLogPath()
{
    if (const fs::path root = testRoot(); !root.empty())
        return root / "agents" / "events.jsonl";

    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        return {};
    return local / "ProjectMan" / "agents" / "events.jsonl";
}

} // namespace pm::cli
