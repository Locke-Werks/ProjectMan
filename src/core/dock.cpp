#include "dock.h"

#include "proc.h"
#include "strutil.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <chrono>
#include <thread>
#include <vector>

namespace pm::dock {
namespace {

// Docked Console's side of the contract, from its src/ids.h. Duplicated rather
// than shared because the two products ship separately: the numbers are the
// interface, and they are documented as additive and never renumbered.
constexpr wchar_t kHostClass[] = L"DockedConsole.Host";
constexpr wchar_t kHostTitle[] = L"Docked Console";
constexpr wchar_t kColumnClass[] = L"DockedConsole.Column";

constexpr ULONG_PTR kSplitCopyDataId = 0x44434B31;   // 'DCK1'
constexpr UINT32    kSplitVersion    = 1;

enum SplitResult : LRESULT {
    kSplitNotHandled = 0,
    kSplitAccepted   = 1,
    kSplitNewColumn  = 2,
    kSplitAtMax      = 3,
    kSplitNoRoom     = 4,
    kSplitRejected   = 5,
    kSplitUnnamed    = 6,
};

#pragma pack(push, 4)
struct SplitRequest {
    UINT32 version;
    UINT32 column_id;
    UINT32 cwd_offset;
    UINT32 cwd_length;
    UINT32 cmd_offset;
    UINT32 cmd_length;
};
#pragma pack(pop)

HWND findHost() { return FindWindowW(kHostClass, kHostTitle); }

struct ColumnCount {
    int n = 0;
};

BOOL CALLBACK countColumn(HWND child, LPARAM param)
{
    wchar_t cls[64] = {};
    if (GetClassNameW(child, cls, static_cast<int>(std::size(cls)))
        && wcscmp(cls, kColumnClass) == 0) {
        ++reinterpret_cast<ColumnCount*>(param)->n;
    }
    return TRUE;
}

bool processElevated(DWORD pid)
{
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        // Failing to open it is itself evidence the target is above us.
        return true;
    }

    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION info{};
        DWORD got = 0;
        if (GetTokenInformation(token, TokenElevation, &info, sizeof(info), &got))
            elevated = info.TokenIsElevated != 0;
        CloseHandle(token);
    }
    CloseHandle(proc);
    return elevated;
}

fs::path arpInstallLocation()
{
    // Forge writes an ARP entry keyed by the product name. Machine scope first,
    // then per-user, because the same product can be installed either way.
    for (HKEY root : { HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER }) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root,
                          L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
                          L"Docked Console",
                          0, KEY_READ, &key)
            != ERROR_SUCCESS) {
            continue;
        }

        wchar_t buf[MAX_PATH * 2] = {};
        DWORD   bytes = sizeof(buf);
        DWORD   type  = 0;
        const LSTATUS s =
            RegQueryValueExW(key, L"InstallLocation", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &bytes);
        RegCloseKey(key);

        if (s == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0])
            return fs::path(buf);
    }
    return {};
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

// Starts a program and lets go of it. Nothing here waits: the caller polls for
// a window instead.
bool spawnDetached(const fs::path& exe, const std::vector<std::wstring>& args,
                   std::string* error)
{
    std::wstring cmdline = widen(quoteArg(exe.string()));
    for (const std::wstring& a : args)
        cmdline += L" " + widen(quoteArg(narrow(a)));
    cmdline.push_back(L'\0');

    const std::wstring exeW = exe.wstring();
    const std::wstring cwdW = exe.parent_path().wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // Scrubbed, because the dock outlives this call and every terminal it ever
    // creates inherits whatever environment it was started with. A dock born
    // inside a Claude Code session would hand that session's child marker to
    // every pane, for as long as it runs.
    std::vector<wchar_t> env = childEnvironment();

    if (!CreateProcessW(exeW.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, env.data(),
                        cwdW.empty() ? nullptr : cwdW.c_str(), &si, &pi)) {
        if (error)
            *error = errorText(GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Sends one request. Returns the raw SplitResult, or kSplitNotHandled when the
// window has gone in the meantime.
LRESULT sendRequest(HWND host, const std::wstring& cwd, const std::wstring& command)
{
    const size_t cwdBytes = cwd.size() * sizeof(wchar_t);
    const size_t cmdBytes = command.size() * sizeof(wchar_t);

    std::vector<char> block(sizeof(SplitRequest) + cwdBytes + cmdBytes);

    SplitRequest header{};
    header.version    = kSplitVersion;
    header.column_id  = 0;   // any column with room; the dock decides
    header.cwd_offset = static_cast<UINT32>(sizeof(SplitRequest));
    header.cwd_length = static_cast<UINT32>(cwd.size());
    header.cmd_offset = static_cast<UINT32>(sizeof(SplitRequest) + cwdBytes);
    header.cmd_length = static_cast<UINT32>(command.size());

    memcpy(block.data(), &header, sizeof(header));
    if (cwdBytes)
        memcpy(block.data() + header.cwd_offset, cwd.data(), cwdBytes);
    if (cmdBytes)
        memcpy(block.data() + header.cmd_offset, command.data(), cmdBytes);

    COPYDATASTRUCT data{};
    data.dwData = kSplitCopyDataId;
    data.cbData = static_cast<DWORD>(block.size());
    data.lpData = block.data();

    DWORD_PTR answer = 0;
    const LRESULT sent = SendMessageTimeoutW(
        host, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
        SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT, 8000, &answer);

    return sent ? static_cast<LRESULT>(answer) : kSplitNotHandled;
}

Status fromSplitResult(LRESULT r)
{
    switch (r) {
    case kSplitAccepted:
    case kSplitNewColumn: return Status::Ok;
    case kSplitAtMax:     return Status::Full;
    case kSplitNoRoom:    return Status::NoRoom;
    case kSplitUnnamed:   return Status::Unnamed;
    case kSplitRejected:  return Status::Rejected;
    default:              return Status::NotHandled;
    }
}

} // namespace

const char* statusText(Status s)
{
    switch (s) {
    case Status::Ok:
        return "docked";
    case Status::NoClaude:
        return "claude.exe was not found, so there is nothing to put in the dock";
    case Status::NotInstalled:
        // The URL is in the message rather than only in the interactive offer,
        // so a piped or scripted run still says where to get it.
        return "Docked Console is not installed. Get it from "
               "https://github.com/Locke-Werks/dockedconsole/releases/latest, "
               "or set dock.exe in the config if it is somewhere unusual";
    case Status::StartFailed:
        return "Docked Console was started and no dock appeared. If it asked for "
               "administrator rights and the prompt was declined, it did not start";
    case Status::NotHandled:
        return "Docked Console is running and did not answer. It is either "
               "shutting down, or is older than 0.4.0 and cannot take a pane";
    case Status::Full:
        return "the dock is full: three columns, each at its pane cap. Close a "
               "pane, or use ENGAGE for a window of its own";
    case Status::NoRoom:
        return "this display has no room for another dock column";
    case Status::Unnamed:
        return "Docked Console has window naming turned off, so its columns "
               "cannot be addressed. Set \"nameWindows\": true and restart it";
    case Status::Rejected:
        return "Docked Console refused the request as malformed, which means the "
               "two are different versions";
    }
    return "the dock refused";
}

const char* releasePage()
{
    return "https://github.com/Locke-Werks/dockedconsole/releases/latest";
}

const char* installerUrl()
{
    return "https://github.com/Locke-Werks/dockedconsole/releases/latest/download/"
           "DockedConsole-Setup.exe";
}

bool openDownload(const char* url, std::string* error)
{
    const HINSTANCE r = ShellExecuteW(nullptr, L"open", widen(url).c_str(),
                                      nullptr, nullptr, SW_SHOWNORMAL);

    // ShellExecuteW predates Win32 error conventions: the return is an error
    // code cast to a handle when it is 32 or less, and a meaningless non-null
    // otherwise. GetLastError is not set on failure.
    const INT_PTR code = reinterpret_cast<INT_PTR>(r);
    if (code <= 32) {
        if (error)
            *error = errorText(static_cast<unsigned long>(code));
        return false;
    }
    return true;
}

State discover()
{
    State st;

    HWND host = findHost();
    if (!host)
        return st;

    st.running = true;
    GetWindowThreadProcessId(host, &st.pid);
    st.elevated = processElevated(st.pid);

    ColumnCount count;
    EnumChildWindows(host, countColumn, reinterpret_cast<LPARAM>(&count));
    st.columns = count.n;
    return st;
}

fs::path resolveExe(const Config& cfg)
{
    std::error_code ec;

    if (!cfg.dockExe.empty() && fs::exists(cfg.dockExe, ec))
        return cfg.dockExe;

    const fs::path arp = arpInstallLocation();
    if (!arp.empty()) {
        const fs::path candidate = arp / "dockedconsole.exe";
        if (fs::exists(candidate, ec))
            return candidate;
    }

    const fs::path pf = knownFolder(FOLDERID_ProgramFiles);
    if (!pf.empty()) {
        const fs::path candidate = pf / "Locke Werks" / "Docked Console" / "dockedconsole.exe";
        if (fs::exists(candidate, ec))
            return candidate;
    }

    wchar_t     buf[MAX_PATH * 2] = {};
    const DWORD n = SearchPathW(nullptr, L"dockedconsole.exe", nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf))
        return fs::path(std::wstring(buf, n));

    return {};
}

Status launch(const LaunchSpec& s, const Config& cfg, std::string* detail)
{
    std::error_code ec;

    const fs::path claude = cfg.resolveClaude();
    if (claude.empty() || !fs::exists(claude, ec)) {
        if (detail)
            *detail = claude.string();
        return Status::NoClaude;
    }

    // Built before the dock is started, because a cold start passes it on the
    // command line as well as sending it as a request.
    std::wstring command = widen(quoteArg(claude.string()));
    for (const std::string& a : claudeArgs(s, cfg))
        command += L" " + widen(quoteArg(a));

    HWND host = findHost();

    if (!host) {
        const fs::path exe = resolveExe(cfg);
        if (exe.empty())
            return Status::NotInstalled;

        // Fire and forget, deliberately not pm::run(). That waits for the
        // process and TERMINATES it on timeout, which for a dock that runs
        // until the user quits it means starting one and immediately killing
        // it. There is nothing to wait for here anyway: the dock relaunches
        // itself elevated through ShellExecuteEx, so the process started here
        // exits within milliseconds while a UAC prompt the user has not
        // answered yet stands between us and the window. Polling for the
        // window is the only honest wait.
        // The dock always gives a new column its own shell, so without this the
        // very first column would be a shell with the requested session in a
        // pane underneath it. Telling it up front makes the column be the thing
        // that was asked for.
        const std::vector<std::wstring> startArgs = {
            L"--pane-dir", s.cwd.wstring(),
            L"--pane-run", command,
        };

        if (!spawnDetached(exe, startArgs, detail))
            return Status::StartFailed;

        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(cfg.dockStartTimeoutMs);
        while (!host && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            host = findHost();
        }
        if (!host)
            return Status::StartFailed;

        // The dock started with the session already in its first column. Sending
        // the request as well would put a second copy in a pane beneath it.
        return Status::Ok;
    }

    return fromSplitResult(sendRequest(host, s.cwd.wstring(), command));
}

} // namespace pm::dock
