#include "config.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <toml++/toml.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace pm {
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

fs::path exeDir()
{
    wchar_t buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return {};
    return fs::path(std::wstring(buf, n)).parent_path();
}

constexpr const char* kFileName = "projectman.toml";

// Every key the parser recognises. A key outside this set is a typo, and a typo
// that is silently ignored means the program quietly does something other than
// what was written down.
const std::set<std::string> kKnownKeys = {
    "root",
    "git_exe",
    "launch.claude",
    "launch.args",
    "launch.model",
    "launch.terminal",
    "launch.terminal_args",
    "scan.threads",
    "scan.timeout_ms",
    "scan.descend_containers",
    "scan.include_plain",
    "scan.exclude",
    "github.enabled",
    "github.owners",
    "github.cache_minutes",
    "dispatch.commit",
    "dispatch.push",
    "dispatch.max_repos",
    "ui.sort",
    "ui.on_exit",
};

void collectKeys(const toml::table& tbl, const std::string& prefix,
                 std::vector<std::string>& out)
{
    for (const auto& [k, v] : tbl) {
        const std::string key =
            prefix.empty() ? std::string(k.str()) : prefix + "." + std::string(k.str());
        if (v.is_table())
            collectKeys(*v.as_table(), key, out);
        else
            out.push_back(key);
    }
}

// Templated on the view type: a non-const toml::table yields
// node_view<node>, a const one yields node_view<const node>, and both need to
// work here.
template <typename View>
std::vector<std::string> stringArray(const View& n)
{
    std::vector<std::string> out;
    if (const toml::array* arr = n.as_array()) {
        for (const toml::node& e : *arr) {
            if (const auto s = e.value<std::string>())
                out.push_back(*s);
        }
    }
    return out;
}

} // namespace

std::string toString(OnChildExit e)
{
    switch (e) {
    case OnChildExit::Quit: return "quit";
    case OnChildExit::Ask:  return "ask";
    case OnChildExit::Return:
    default:                return "return";
    }
}

OnChildExit onChildExitFromString(std::string_view s, bool* ok)
{
    if (ok)
        *ok = true;
    if (iequals(s, "quit"))
        return OnChildExit::Quit;
    if (iequals(s, "ask"))
        return OnChildExit::Ask;
    if (iequals(s, "return"))
        return OnChildExit::Return;
    if (ok)
        *ok = false;
    return OnChildExit::Return;
}

Config Config::defaults()
{
    Config c;

    const fs::path profile = knownFolder(FOLDERID_Profile);
    c.root      = profile / "projects";
    c.claudeExe = profile / ".local" / "bin" / "claude.exe";

    // The two flags the user runs Claude Code with everywhere else.
    c.claudeArgs = { "--dangerously-skip-permissions", "--effort", "max" };

    c.githubOwners = { "lockewerks", "Locke-Werks", "nyxlocke", "jhancuff" };

    // Local backup mirrors and agent worktrees are real directories but not
    // projects, and both would otherwise show up as rows.
    c.exclude = { "copilot-worktrees", "DeadLetter-backup-mirror.git" };

    return c;
}

fs::path Config::perUserPath()
{
    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        return {};
    return local / "ProjectMan" / kFileName;
}

fs::path Config::portablePath()
{
    const fs::path dir = exeDir();
    if (dir.empty())
        return {};
    return dir / kFileName;
}

fs::path Config::filePath()
{
    std::error_code ec;
    const fs::path  portable = portablePath();
    if (!portable.empty() && fs::exists(portable, ec))
        return portable;
    return perUserPath();
}

fs::path Config::resolveClaude() const
{
    std::error_code ec;
    if (!claudeExe.empty() && fs::exists(claudeExe, ec))
        return claudeExe;

    wchar_t     buf[MAX_PATH * 2] = {};
    const DWORD n = SearchPathW(nullptr, L"claude.exe", nullptr,
                                static_cast<DWORD>(std::size(buf)), buf, nullptr);
    if (n > 0 && n < std::size(buf))
        return fs::path(std::wstring(buf, n));

    return claudeExe;   // report the configured path in the error
}

fs::path Config::resolveTerminal() const
{
    if (!terminalExe.empty())
        return terminalExe;

    const fs::path local = knownFolder(FOLDERID_LocalAppData);
    if (local.empty())
        return {};

    // wt.exe is an app execution alias: a zero-length reparse point. Any check
    // based on file size reports it missing, so attributes are the only correct
    // existence test. This trap is documented in dockedconsole's terminal.cpp.
    const fs::path wt = local / "Microsoft" / "WindowsApps" / "wt.exe";
    if (GetFileAttributesW(wt.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};

    return wt;
}

Config Config::load(ConfigStatus* status, std::string* detail)
{
    Config c = defaults();

    const fs::path path = filePath();
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec)) {
        if (status)
            *status = ConfigStatus::Missing;
        return c;
    }

    toml::table tbl;
    try {
        tbl = toml::parse_file(path.string());
    } catch (const toml::parse_error& e) {
        if (status)
            *status = ConfigStatus::Unreadable;
        if (detail) {
            std::ostringstream os;
            os << path.string() << ":" << e.source().begin.line << ": " << e.description();
            *detail = os.str();
        }
        return c;   // defaults in memory; the file is left exactly as it is
    }

    // A typo stops the program with a message instead of silently doing
    // something else for an hour.
    std::vector<std::string> present;
    collectKeys(tbl, {}, present);
    for (const std::string& k : present) {
        if (!kKnownKeys.count(k)) {
            if (status)
                *status = ConfigStatus::Unreadable;
            if (detail)
                *detail = path.string() + ": unknown key \"" + k + "\"";
            return Config::defaults();
        }
    }

    const auto str = [&](const char* a, const char* b = nullptr) -> std::optional<std::string> {
        return b ? tbl[a][b].value<std::string>() : tbl[a].value<std::string>();
    };

    if (const auto v = str("root"))            c.root      = fs::path(widen(*v));
    if (const auto v = str("git_exe"))         c.gitExe    = fs::path(widen(*v));
    if (const auto v = str("launch", "claude"))   c.claudeExe   = fs::path(widen(*v));
    if (const auto v = str("launch", "model"))    c.model       = *v;
    if (const auto v = str("launch", "terminal")) c.terminalExe = fs::path(widen(*v));
    if (const auto v = str("launch", "terminal_args")) c.terminalArgs = *v;

    if (tbl["launch"]["args"].is_array())
        c.claudeArgs = stringArray(tbl["launch"]["args"]);

    c.scanThreads       = tbl["scan"]["threads"].value_or(c.scanThreads);
    c.probeTimeoutMs    = tbl["scan"]["timeout_ms"].value_or(c.probeTimeoutMs);
    c.descendContainers = tbl["scan"]["descend_containers"].value_or(c.descendContainers);
    c.includePlain      = tbl["scan"]["include_plain"].value_or(c.includePlain);
    if (tbl["scan"]["exclude"].is_array())
        c.exclude = stringArray(tbl["scan"]["exclude"]);

    c.githubEnabled      = tbl["github"]["enabled"].value_or(c.githubEnabled);
    c.githubCacheMinutes = tbl["github"]["cache_minutes"].value_or(c.githubCacheMinutes);
    if (tbl["github"]["owners"].is_array())
        c.githubOwners = stringArray(tbl["github"]["owners"]);

    c.dispatchCommit   = tbl["dispatch"]["commit"].value_or(c.dispatchCommit);
    c.dispatchPush     = tbl["dispatch"]["push"].value_or(c.dispatchPush);
    c.dispatchMaxRepos = tbl["dispatch"]["max_repos"].value_or(c.dispatchMaxRepos);

    if (const auto v = str("ui", "sort"))
        c.sort = *v;
    if (const auto v = str("ui", "on_exit")) {
        bool ok = false;
        c.onExit = onChildExitFromString(*v, &ok);
        if (!ok) {
            if (status)
                *status = ConfigStatus::Unreadable;
            if (detail)
                *detail = path.string() + ": ui.on_exit must be return, quit or ask";
            return Config::defaults();
        }
    }

    if (status)
        *status = ConfigStatus::Loaded;
    return c;
}

bool Config::save(std::string* error) const
{
    const fs::path path = perUserPath();
    if (path.empty()) {
        if (error)
            *error = "cannot resolve %LOCALAPPDATA%";
        return false;
    }

    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);

    // TOML literal strings for every path. In a basic string "{InstallDir}\bin"
    // parses \b as a backspace, which is the same trap Forge's config schema
    // documents for installer.toml.
    std::ostringstream os;
    os << "# ProjectMan configuration.\n"
       << "# A copy of this file beside projectman.exe takes precedence over this one.\n"
       << "# Unknown keys are rejected rather than ignored.\n\n";

    os << "root = '" << root.string() << "'\n\n";

    os << "[launch]\n";
    os << "claude = '" << claudeExe.string() << "'\n";
    os << "args   = [";
    for (size_t i = 0; i < claudeArgs.size(); ++i)
        os << (i ? ", " : "") << '"' << claudeArgs[i] << '"';
    os << "]\n";
    os << "terminal_args = \"" << terminalArgs << "\"\n\n";

    os << "[scan]\n";
    os << "threads            = " << scanThreads << "\n";
    os << "timeout_ms         = " << probeTimeoutMs << "\n";
    os << "descend_containers = " << (descendContainers ? "true" : "false") << "\n";
    os << "include_plain      = " << (includePlain ? "true" : "false") << "\n";
    os << "exclude            = [";
    for (size_t i = 0; i < exclude.size(); ++i)
        os << (i ? ", " : "") << '"' << exclude[i] << '"';
    os << "]\n\n";

    os << "[github]\n";
    os << "enabled       = " << (githubEnabled ? "true" : "false") << "\n";
    os << "cache_minutes = " << githubCacheMinutes << "\n";
    os << "owners        = [";
    for (size_t i = 0; i < githubOwners.size(); ++i)
        os << (i ? ", " : "") << '"' << githubOwners[i] << '"';
    os << "]\n\n";

    os << "# The multi-repo orchestrator. push stays off by default: a run that\n"
       << "# commits is reviewable with git log and reversible with git reset,\n"
       << "# whereas one that pushes has already left the machine.\n";
    os << "[dispatch]\n";
    os << "commit    = " << (dispatchCommit ? "true" : "false") << "\n";
    os << "push      = " << (dispatchPush ? "true" : "false") << "\n";
    os << "max_repos = " << dispatchMaxRepos << "\n\n";

    os << "[ui]\n";
    os << "sort    = \"" << sort << "\"\n";
    os << "on_exit = \"" << toString(onExit) << "\"\n";

    const std::string body = os.str();

    // Write to a sibling temp file and rename over the target, so an
    // interrupted write cannot truncate a good config.
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error)
                *error = "cannot write " + tmp.string();
            return false;
        }
        f.write(body.data(), static_cast<std::streamsize>(body.size()));
    }

    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error)
            *error = "cannot replace " + path.string() + ": " + errorText(GetLastError());
        return false;
    }

    return true;
}

} // namespace pm
