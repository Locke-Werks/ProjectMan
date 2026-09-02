#include "proc.h"
#include "strutil.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace pm {
namespace {

// A handle that closes itself. CreateProcessW leaves several handles in flight
// and every early return has to release them.
class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() { reset(); }

    Handle(const Handle&)            = delete;
    Handle& operator=(const Handle&) = delete;

    void reset(HANDLE h = nullptr)
    {
        if (h_ && h_ != INVALID_HANDLE_VALUE)
            CloseHandle(h_);
        h_ = h;
    }

    HANDLE get() const { return h_; }
    bool valid() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_ = nullptr;
};

// Drain a pipe until the far end closes. Runs on its own thread so stdout and
// stderr cannot deadlock against each other: one filling its 64 KB buffer while
// the reader is blocked on the other is a real hang, not a theoretical one.
void drain(HANDLE pipe, std::string* sink)
{
    char buf[8192];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;
        sink->append(buf, got);
    }
}

// A line that is still growing after this much is not a line, it is a stream
// with no newlines in it. Hand it over rather than holding it forever.
constexpr size_t kMaxLineBytes = 64u * 1024u * 1024u;

// The same drain, but delivered a line at a time, with '\n' removed and a
// trailing '\r' stripped so a child writing CRLF reads the same as one that
// does not. Empty lines are dropped: nothing this feeds wants them.
void drainLines(HANDLE pipe, Stream which, LineSink* sink)
{
    const auto emit = [&](std::string line) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            sink->onLine(which, std::move(line));
    };

    std::string pending;
    char        buf[8192];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(pipe, buf, sizeof(buf), &got, nullptr) || got == 0)
            break;
        pending.append(buf, got);

        // Lines span reads: a 200 KB tool result arrives in 25 pieces and is
        // still one line.
        size_t start = 0;
        for (;;) {
            const size_t nl = pending.find('\n', start);
            if (nl == std::string::npos)
                break;
            emit(pending.substr(start, nl - start));
            start = nl + 1;
        }
        pending.erase(0, start);

        if (pending.size() > kMaxLineBytes) {
            emit(std::move(pending));
            pending.clear();
        }
    }
    if (!pending.empty())
        emit(std::move(pending));
}

// One pipe with exactly one inheritable end. Leaving the parent's end
// inheritable means the child holds a copy, the pipe never reports EOF, and the
// drain blocks forever.
bool makePipe(Handle* parentEnd, Handle* childEnd, bool childWrites)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0))
        return false;

    const HANDLE parent = childWrites ? r : w;
    const HANDLE child  = childWrites ? w : r;
    SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0);

    parentEnd->reset(parent);
    childEnd->reset(child);
    return true;
}

struct Spawned {
    Handle process;
    Handle thread;
    Handle job;   // empty when the child could not be put in one
};

// Starts exe with args, hidden, with exactly the three standard handles named
// here inherited and nothing else.
//
// The handle list is not tidiness. Without it every inheritable handle in this
// process goes to every child, and a long-lived child then holds the pipe ends
// of every other child started while it runs: a git probe's reader would sit on
// a pipe that claude.exe also holds, until claude.exe exits.
bool spawn(const fs::path& exe, const std::vector<std::string>& args, const fs::path& cwd,
           HANDLE in, HANDLE out, HANDLE err, bool inJob, Spawned* child, std::string* error)
{
    std::string cmdline = quoteArg(exe.string());
    for (const std::string& a : args) {
        cmdline.push_back(' ');
        cmdline += quoteArg(a);
    }

    // CreateProcessW writes into lpCommandLine, so it must be a writable buffer.
    // Passing a literal is the classic way to earn an access violation here.
    std::wstring mutableCmd = widen(cmdline);
    mutableCmd.push_back(L'\0');

    const std::wstring exeW = exe.wstring();
    const std::wstring cwdW = cwd.wstring();

    HANDLE handles[3] = {};
    DWORD  count      = 0;
    for (HANDLE h : { in, out, err }) {
        if (!h || h == INVALID_HANDLE_VALUE)
            continue;
        if (std::find(handles, handles + count, h) != handles + count)
            continue;   // the list refuses duplicates
        handles[count++] = h;
    }

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<char> buffer(size);
    auto* list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &size)) {
        *error = "cannot build the handle list: " + errorText(GetLastError());
        return false;
    }

    struct ListGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST list;
        ~ListGuard() { DeleteProcThreadAttributeList(list); }
    } guard{ list };

    if (count > 0
        && !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                      count * sizeof(HANDLE), nullptr, nullptr)) {
        *error = "cannot restrict inherited handles: " + errorText(GetLastError());
        return false;
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb          = sizeof(si);
    si.StartupInfo.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.StartupInfo.wShowWindow = SW_HIDE;
    si.StartupInfo.hStdInput   = in;
    si.StartupInfo.hStdOutput  = out;
    si.StartupInfo.hStdError   = err;
    si.lpAttributeList         = list;

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> env = childEnvironment();

    // Suspended when a job is wanted, so the child cannot start anything of
    // its own before it is inside the job.
    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT;
    if (inJob)
        flags |= CREATE_SUSPENDED;

    const BOOL ok = CreateProcessW(
        exeW.c_str(), mutableCmd.data(), nullptr, nullptr,
        TRUE,   // inherit, and the attribute list says exactly what
        flags, env.data(), cwd.empty() ? nullptr : cwdW.c_str(),
        &si.StartupInfo, &pi);

    if (!ok) {
        *error = errorText(GetLastError());
        return false;
    }

    child->process.reset(pi.hProcess);
    child->thread.reset(pi.hThread);

    if (inJob) {
        // Kill-on-close, so a ProjectMan that dies takes the run with it rather
        // than leaving a claude.exe working away with nobody reading it.
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                        sizeof(limits))
                && AssignProcessToJobObject(job, pi.hProcess)) {
                child->job.reset(job);
            } else {
                // Refused, which a job this process is already inside can
                // arrange. The run still works; stopping it reaches one
                // process less deep.
                CloseHandle(job);
            }
        }
        ResumeThread(pi.hThread);
    }

    return true;
}

void killTree(const Spawned& child)
{
    if (child.job.valid())
        TerminateJobObject(child.job.get(), 1);
    else
        TerminateProcess(child.process.get(), 1);
}

} // namespace

std::string errorText(unsigned long code)
{
    LPWSTR buf = nullptr;
    const DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buf), 0, nullptr);

    std::string msg = n ? narrow(std::wstring_view(buf, n))
                        : ("error " + std::to_string(code));
    if (buf)
        LocalFree(buf);

    return std::string(trim(msg));
}

std::string quoteArg(std::string_view arg)
{
    const bool needsQuotes =
        arg.empty() || arg.find_first_of(" \t\n\v\"") != std::string_view::npos;
    if (!needsQuotes)
        return std::string(arg);

    std::string out;
    out.reserve(arg.size() + 8);
    out.push_back('"');

    int backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            // Backslashes immediately before a quote must be doubled, then the
            // quote itself escaped.
            out.append(static_cast<size_t>(backslashes) * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(static_cast<size_t>(backslashes), '\\');
            out.push_back(c);
        }
        backslashes = 0;
    }

    // Trailing backslashes would otherwise escape the closing quote.
    out.append(static_cast<size_t>(backslashes) * 2, '\\');
    out.push_back('"');
    return out;
}

std::vector<wchar_t> childEnvironment()
{
    // Session-scoped runtime state, not user configuration. Anything else
    // beginning CLAUDE_CODE_ is left alone, because a person may have set it
    // deliberately and it is not ours to drop.
    static const wchar_t* kStrip[] = {
        L"CLAUDE_CODE_CHILD_SESSION",
        L"CLAUDE_CODE_SESSION_ID",
        L"CLAUDE_CODE_BRIDGE_SESSION_ID",
        L"CLAUDE_CODE_MESSAGING_SOCKET",
        L"CLAUDE_CODE_MESSAGING_TOKEN",
        L"CLAUDE_CODE_ENTRYPOINT",
        L"CLAUDE_CODE_EXECPATH",
    };

    std::vector<wchar_t> out;

    wchar_t* block = GetEnvironmentStringsW();
    if (!block) {
        out.push_back(L'\0');
        out.push_back(L'\0');
        return out;
    }

    for (const wchar_t* entry = block; *entry;) {
        const size_t len = wcslen(entry);

        const wchar_t* eq = wcschr(entry, L'=');
        bool drop = false;
        if (eq && eq != entry) {
            const size_t nameLen = static_cast<size_t>(eq - entry);
            for (const wchar_t* name : kStrip) {
                if (wcslen(name) == nameLen
                    && _wcsnicmp(entry, name, nameLen) == 0) {
                    drop = true;
                    break;
                }
            }
        }

        if (!drop)
            out.insert(out.end(), entry, entry + len + 1);   // keep the NUL

        entry += len + 1;
    }

    FreeEnvironmentStringsW(block);

    // A block of nothing but the terminator still has to be double-NUL.
    if (out.empty())
        out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}

ProcResult run(const fs::path& exe, const std::vector<std::string>& args,
               const fs::path& cwd, int timeoutMs)
{
    using clock = std::chrono::steady_clock;
    const auto began = clock::now();

    ProcResult result;

    Handle outRead, outWrite, errRead, errWrite;
    if (!makePipe(&outRead, &outWrite, /*childWrites=*/true)
        || !makePipe(&errRead, &errWrite, /*childWrites=*/true)) {
        result.launchError = "cannot create pipe: " + errorText(GetLastError());
        return result;
    }

    Spawned child;
    if (!spawn(exe, args, cwd, nullptr, outWrite.get(), errWrite.get(), /*inJob=*/false,
               &child, &result.launchError)) {
        return result;
    }
    result.started = true;

    // The parent's copies of the write ends must go, or the pipes never reach
    // EOF and both drain threads hang after the child exits.
    outWrite.reset();
    errWrite.reset();

    std::thread outThread(drain, outRead.get(), &result.out);
    std::thread errThread(drain, errRead.get(), &result.err);

    const DWORD waited = WaitForSingleObject(
        child.process.get(), timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : INFINITE);

    if (waited == WAIT_TIMEOUT) {
        result.timedOut = true;
        // Killing the child closes its handles, which unblocks both readers.
        TerminateProcess(child.process.get(), 1);
        WaitForSingleObject(child.process.get(), 5000);
    }

    outThread.join();
    errThread.join();

    DWORD code = 0;
    if (GetExitCodeProcess(child.process.get(), &code))
        result.exitCode = static_cast<int>(code);

    result.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began).count());
    return result;
}

StreamResult runStreaming(const StreamSpec& spec, CancelToken& token, LineSink& sink)
{
    using clock = std::chrono::steady_clock;
    const auto began = clock::now();

    StreamResult result;

    Handle inWrite, inRead, outRead, outWrite, errRead, errWrite;
    if (!makePipe(&inWrite, &inRead, /*childWrites=*/false)
        || !makePipe(&outRead, &outWrite, /*childWrites=*/true)
        || !makePipe(&errRead, &errWrite, /*childWrites=*/true)) {
        result.launchError = "cannot create pipe: " + errorText(GetLastError());
        return result;
    }

    Spawned child;
    if (!spawn(spec.exe, spec.args, spec.cwd, inRead.get(), outWrite.get(), errWrite.get(),
               /*inJob=*/true, &child, &result.launchError)) {
        return result;
    }
    result.started = true;

    // The parent's copies of the child's ends must go, or nothing reaches EOF.
    inRead.reset();
    outWrite.reset();
    errWrite.reset();

    // stdin on a thread of its own. A briefing is larger than the pipe buffer,
    // so the write blocks until the child reads, and a child that dies before
    // it reads would otherwise block this thread for good. Only this thread
    // touches inWrite: closing it is what tells the child the prompt has ended.
    std::thread writer([&spec, &inWrite] {
        const char* p    = spec.stdinData.data();
        size_t      left = spec.stdinData.size();
        while (left > 0) {
            DWORD wrote = 0;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 64u * 1024u));
            if (!WriteFile(inWrite.get(), p, chunk, &wrote, nullptr) || wrote == 0)
                break;
            p += wrote;
            left -= wrote;
        }
        inWrite.reset();
    });

    std::atomic<bool> outDone{ false }, errDone{ false };
    std::thread outThread([&] {
        drainLines(outRead.get(), Stream::Out, &sink);
        outDone.store(true);
    });
    std::thread errThread([&] {
        drainLines(errRead.get(), Stream::Err, &sink);
        errDone.store(true);
    });

    for (;;) {
        if (WaitForSingleObject(child.process.get(), 100) == WAIT_OBJECT_0)
            break;
        if (token.stop_requested()) {
            result.stopped = true;
            killTree(child);
            WaitForSingleObject(child.process.get(), 5000);
            break;
        }
    }

    // A grandchild still holding the write end keeps the readers waiting after
    // the child itself has gone. Give it a moment, then take the whole job
    // down, which closes the pipes and lets the readers finish.
    const auto grace = clock::now() + std::chrono::seconds(2);
    while (!(outDone.load() && errDone.load()) && clock::now() < grace)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!(outDone.load() && errDone.load()))
        killTree(child);

    writer.join();
    outThread.join();
    errThread.join();

    DWORD code = 0;
    if (GetExitCodeProcess(child.process.get(), &code))
        result.exitCode = static_cast<int>(code);

    result.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began).count());
    return result;
}

} // namespace pm
