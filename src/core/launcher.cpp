#include "launcher.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>

#include <atomic>
#include <fstream>
#include <system_error>

namespace pm {
namespace {

std::atomic<bool> g_childLive{ false };

// Windows caps a command line at 32767 characters. A briefing spanning several
// repositories can approach that, so anything large goes to a file and the
// prompt points at it instead of risking a truncated or rejected launch.
constexpr size_t kMaxInlinePrompt = 8000;

fs::path spillPrompt(const std::string& text)
{
    wchar_t     buf[MAX_PATH] = {};
    const DWORD n = GetTempPathW(static_cast<DWORD>(std::size(buf)), buf);
    if (n == 0 || n >= std::size(buf))
        return {};

    const fs::path dir = fs::path(std::wstring(buf, n)) / "ProjectMan";
    std::error_code ec;
    fs::create_directories(dir, ec);

    const fs::path out = dir / "dispatch-briefing.md";
    std::ofstream  f(out, std::ios::binary | std::ios::trunc);
    if (!f)
        return {};
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out;
}

} // namespace

bool childLive() { return g_childLive.load(std::memory_order_acquire); }

std::vector<std::string> claudeArgs(const LaunchSpec& s, const Config& cfg)
{
    std::vector<std::string> args;

    // Composed from the settings rather than stored as a literal command line,
    // so the autonomy ladder actually reaches the session. At Suggest, skipping
    // permission checks would contradict the whole rung.
    if (cfg.resolvedSkipPermissions())
        args.emplace_back("--dangerously-skip-permissions");

    if (!cfg.effort.empty()) {
        args.emplace_back("--effort");
        args.push_back(cfg.effort);
    }

    args.insert(args.end(), cfg.extraArgs.begin(), cfg.extraArgs.end());

    switch (s.mode) {
    case LaunchMode::Continue:
        args.emplace_back("--continue");
        break;
    case LaunchMode::Resume:
        args.emplace_back("--resume");
        if (!s.resumeId.empty())
            args.push_back(s.resumeId);
        break;
    case LaunchMode::New:
        break;
    }

    const std::string& model = s.model.empty() ? cfg.model : s.model;
    if (!model.empty()) {
        args.emplace_back("--model");
        args.push_back(model);
    }

    for (const fs::path& d : s.addDirs) {
        args.emplace_back("--add-dir");
        args.push_back(d.string());
    }

    args.insert(args.end(), s.extra.begin(), s.extra.end());

    // The prompt is positional and must come last, or claude reads the flags
    // that follow it as part of the prompt.
    if (!s.prompt.empty())
        args.push_back(s.prompt);

    return args;
}

HandoffResult handoff(const LaunchSpec& s, const Config& cfg)
{
    HandoffResult r;

    const fs::path claude = cfg.resolveClaude();
    std::error_code ec;
    if (claude.empty() || !fs::exists(claude, ec)) {
        r.error = "claude.exe not found at " + claude.string();
        return r;
    }

    std::string cmdline = quoteArg(claude.string());
    for (const std::string& a : claudeArgs(s, cfg))
        cmdline += " " + quoteArg(a);

    std::wstring mutableCmd = widen(cmdline);
    mutableCmd.push_back(L'\0');

    const std::wstring exeW = claude.wstring();
    const std::wstring cwdW = s.cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    // STARTF_USESTDHANDLES is deliberately NOT set. Leaving it unset hands the
    // child the console's own handles rather than duplicates, which is what a
    // terminal handoff needs, and it degrades correctly if a standard handle
    // happens to be redirected.

    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> env = childEnvironment();

    g_childLive.store(true, std::memory_order_release);

    const BOOL ok = CreateProcessW(
        exeW.c_str(),
        mutableCmd.data(),
        nullptr, nullptr,
        TRUE,        // inherit handles, so the child shares this console
        CREATE_UNICODE_ENVIRONMENT,
                     // no CREATE_NEW_CONSOLE: that would open a second window.
                     // no CREATE_NEW_PROCESS_GROUP either: with it the child
                     // lands in its own group and the console stops delivering
                     // CTRL_C_EVENT to it, so Ctrl+C silently dies inside claude.
        env.data(),  // scrubbed, so the session is not born a nested child
        cwdW.empty() ? nullptr : cwdW.c_str(),   // the whole point of the call
        &si, &pi);

    if (!ok) {
        g_childLive.store(false, std::memory_order_release);
        r.error = "could not start " + claude.string() + ": "
                + errorText(GetLastError());
        return r;
    }

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    g_childLive.store(false, std::memory_order_release);

    r.started  = true;
    r.exitCode = static_cast<int>(code);
    return r;
}

bool openShellInTerminal(const fs::path& cwd, const Config& cfg, std::string* error)
{
    const fs::path wt = cfg.resolveTerminal();
    if (wt.empty()) {
        if (error)
            *error = "Windows Terminal was not found";
        return false;
    }

    // No "--" and no command: wt starts the default profile, which is the whole
    // point of this being a separate function.
    std::string cmdline = quoteArg(wt.string()) + " " + cfg.terminalArgs + " -d "
                        + quoteArg(cwd.string());

    std::wstring mutableCmd = widen(cmdline);
    mutableCmd.push_back(L'\0');

    const std::wstring exeW = wt.wstring();
    const std::wstring cwdW = cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> env = childEnvironment();

    if (!CreateProcessW(exeW.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, env.data(),
                        cwdW.empty() ? nullptr : cwdW.c_str(), &si, &pi)) {
        if (error)
            *error = "could not start " + wt.string() + ": " + errorText(GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool openInTerminal(const LaunchSpec& s, const Config& cfg, std::string* error)
{
    const fs::path wt     = cfg.resolveTerminal();
    const fs::path claude = cfg.resolveClaude();

    std::error_code ec;
    if (claude.empty() || !fs::exists(claude, ec)) {
        if (error)
            *error = "claude.exe not found at " + claude.string();
        return false;
    }

    std::string cmdline;
    std::wstring exeW;

    if (!wt.empty()) {
        // wt -w new -d "<path>" -- "<claude>" [args]
        //
        // The "--" matters: without it wt parses claude's own -c and -r as its
        // own options and fails. -d must precede the command.
        cmdline = quoteArg(wt.string()) + " " + cfg.terminalArgs + " -d "
                + quoteArg(s.cwd.string()) + " -- " + quoteArg(claude.string());
        exeW = wt.wstring();
    } else {
        // No Windows Terminal. A console of our own is a worse experience but
        // it is not a failure.
        cmdline = quoteArg(claude.string());
        exeW    = claude.wstring();
    }

    for (const std::string& a : claudeArgs(s, cfg))
        cmdline += " " + quoteArg(a);

    std::wstring mutableCmd = widen(cmdline);
    mutableCmd.push_back(L'\0');

    const std::wstring cwdW = s.cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> env = childEnvironment();

    const BOOL ok = CreateProcessW(
        exeW.c_str(), mutableCmd.data(), nullptr, nullptr,
        FALSE,
        (wt.empty() ? CREATE_NEW_CONSOLE : 0u) | CREATE_UNICODE_ENVIRONMENT,
        env.data(),
        cwdW.empty() ? nullptr : cwdW.c_str(),
        &si, &pi);

    if (!ok) {
        if (error) {
            const std::string what = exeW.empty() ? std::string("terminal") : narrow(exeW);
            *error = "could not start " + what + ": " + errorText(GetLastError());
        }
        return false;
    }

    // Nothing to wait on. wt.exe is a stub that hands the request to an
    // already-running WindowsTerminal monarch and exits immediately, so its
    // exit code says nothing about whether the session started.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

LaunchSpec dispatchSpec(const DispatchPlan& plan, const Config& cfg)
{
    LaunchSpec s;
    s.cwd     = cfg.root;
    s.mode    = LaunchMode::New;
    s.addDirs = plan.repos;

    if (plan.briefing.size() <= kMaxInlinePrompt) {
        s.prompt = plan.briefing;
    } else {
        const fs::path spill = spillPrompt(plan.briefing);
        s.prompt = spill.empty()
            ? plan.briefing.substr(0, kMaxInlinePrompt)
            : "Read " + spill.string()
                  + " and follow it. It is the briefing for this session.";
    }

    return s;
}

} // namespace pm
