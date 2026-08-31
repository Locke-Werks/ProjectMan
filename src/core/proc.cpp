#include "proc.h"
#include "strutil.h"

#include <windows.h>

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

bool makePipe(Handle* readEnd, Handle* writeEnd)
{
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE r = nullptr, w = nullptr;
    if (!CreatePipe(&r, &w, &sa, 0))
        return false;

    // The child inherits the write end only. Leaving the read end inheritable
    // means the child holds a copy, the pipe never reports EOF, and the drain
    // above blocks forever.
    SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);

    readEnd->reset(r);
    writeEnd->reset(w);
    return true;
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

ProcResult run(const fs::path& exe, const std::vector<std::string>& args,
               const fs::path& cwd, int timeoutMs)
{
    using clock = std::chrono::steady_clock;
    const auto began = clock::now();

    ProcResult result;

    Handle outRead, outWrite, errRead, errWrite;
    if (!makePipe(&outRead, &outWrite) || !makePipe(&errRead, &errWrite)) {
        result.launchError = "cannot create pipe: " + errorText(GetLastError());
        return result;
    }

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

    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput   = nullptr;
    si.hStdOutput  = outWrite.get();
    si.hStdError   = errWrite.get();

    PROCESS_INFORMATION pi{};

    const BOOL ok = CreateProcessW(
        exeW.c_str(),
        mutableCmd.data(),
        nullptr, nullptr,
        TRUE,                       // inherit, so the child gets the pipe ends
        CREATE_NO_WINDOW,
        nullptr,
        cwd.empty() ? nullptr : cwdW.c_str(),
        &si, &pi);

    if (!ok) {
        result.launchError = errorText(GetLastError());
        return result;
    }

    Handle proc(pi.hProcess);
    Handle thread(pi.hThread);
    result.started = true;

    // The parent's copies of the write ends must go, or the pipes never reach
    // EOF and both drain threads hang after the child exits.
    outWrite.reset();
    errWrite.reset();

    std::thread outThread(drain, outRead.get(), &result.out);
    std::thread errThread(drain, errRead.get(), &result.err);

    const DWORD waited = WaitForSingleObject(
        proc.get(), timeoutMs > 0 ? static_cast<DWORD>(timeoutMs) : INFINITE);

    if (waited == WAIT_TIMEOUT) {
        result.timedOut = true;
        // Killing the child closes its handles, which unblocks both readers.
        TerminateProcess(proc.get(), 1);
        WaitForSingleObject(proc.get(), 5000);
    }

    outThread.join();
    errThread.join();

    DWORD code = 0;
    if (GetExitCodeProcess(proc.get(), &code))
        result.exitCode = static_cast<int>(code);

    result.elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began).count());
    return result;
}

} // namespace pm
