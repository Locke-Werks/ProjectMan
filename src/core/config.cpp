#include "config.h"
#include "proc.h"
#include "strutil.h"

#include <windows.h>
#include <shlobj.h>

#include <toml++/toml.hpp>

#include <algorithm>
#include <cstring>
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
    wchar_t     buf[MAX_PATH * 2] = {};
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf))
        return {};
    return fs::path(std::wstring(buf, n)).parent_path();
}

constexpr const char* kFileName = "projectman.toml";

// Every key the parser recognises. A key outside this set is a typo, and a typo
// that is silently ignored means the program quietly does something other than
// what was written down.
//
// launch.args is still here on purpose. It was the whole launch surface before
// the autonomy ladder existed, and a config written by an earlier build must
// keep loading rather than being rejected as unknown. See migrateLaunchArgs.
const std::set<std::string> kKnownKeys = {
    "root",
    "autonomy",
    "git_exe",
    "launch.claude",
    "launch.model",
    "launch.effort",
    "launch.skip_permissions",
    "launch.extra_args",
    "launch.args",
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
    "dock.exe",
    "dock.start_timeout_ms",
    "dispatch.max_repos",
    "dispatch.max_items",
    // Superseded by the autonomy ladder. Still recognised so a config written
    // by an earlier build loads rather than being rejected as a typo.
    "dispatch.commit",
    "dispatch.push",
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

// Templated on the view type: a non-const toml::table yields node_view<node>,
// a const one yields node_view<const node>, and both need to work here.
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

// An older config carried the whole launch line in launch.args. Pull the flags
// the settings surface now owns out of it and leave the rest as extras, so an
// upgrade neither loses a setting nor passes --effort twice.
void migrateLaunchArgs(Config& cfg, const std::vector<std::string>& args)
{
    cfg.extraArgs.clear();

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];

        if (a == "--dangerously-skip-permissions") {
            // Deliberately not marked explicit. Every config written before the
            // ladder existed carried this flag, so pinning it here would leave
            // the autonomy setting unable to take it away again: dropping to
            // suggest would still hand the session a blanket permission bypass.
            // The ladder produces the same flag at commit and above anyway, so
            // letting it govern preserves the old behaviour and restores the
            // lower rungs.
            cfg.skipPermissions = true;
            continue;
        }
        if (a == "--effort" && i + 1 < args.size()) {
            cfg.effort = args[++i];
            continue;
        }
        if (a == "--model" && i + 1 < args.size()) {
            cfg.model = args[++i];
            continue;
        }
        cfg.extraArgs.push_back(a);
    }
}

std::string joinList(const std::vector<std::string>& v)
{
    std::string out;
    for (size_t i = 0; i < v.size(); ++i)
        out += (i ? ", " : "") + v[i];
    return out;
}

std::vector<std::string> splitList(std::string_view s)
{
    std::vector<std::string> out;
    size_t                   start = 0;
    while (start <= s.size()) {
        const size_t comma = s.find(',', start);
        const size_t end   = comma == std::string_view::npos ? s.size() : comma;
        const std::string_view piece = trim(s.substr(start, end - start));
        if (!piece.empty())
            out.emplace_back(piece);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return out;
}

bool parseBool(std::string_view s, bool* out)
{
    if (iequals(s, "true") || iequals(s, "on") || iequals(s, "yes") || s == "1") {
        *out = true;
        return true;
    }
    if (iequals(s, "false") || iequals(s, "off") || iequals(s, "no") || s == "0") {
        *out = false;
        return true;
    }
    return false;
}

} // namespace

// ------------------------------------------------------------------ autonomy

const char* autonomyName(Autonomy a)
{
    switch (a) {
    case Autonomy::Suggest: return "suggest";
    case Autonomy::Write:   return "write";
    case Autonomy::Commit:  return "commit";
    case Autonomy::Push:    return "push";
    case Autonomy::Full:    return "full";
    }
    return "commit";
}

const char* autonomyLabel(Autonomy a)
{
    switch (a) {
    case Autonomy::Suggest: return "SUGGEST";
    case Autonomy::Write:   return "WRITE";
    case Autonomy::Commit:  return "COMMIT";
    case Autonomy::Push:    return "PUSH";
    case Autonomy::Full:    return "FULL";
    }
    return "COMMIT";
}

const char* autonomySummary(Autonomy a)
{
    switch (a) {
    case Autonomy::Suggest:
        return "Read and report. Changes nothing, and asks before every tool.";
    case Autonomy::Write:
        return "Edit the working tree. Every change stays visible in git diff.";
    case Autonomy::Commit:
        return "Edit and commit. Nothing leaves the machine; git reset undoes it.";
    case Autonomy::Push:
        return "Commit and push a branch. Work leaves the machine.";
    case Autonomy::Full:
        return "Push and open pull requests. Reviewable, but public.";
    }
    return "";
}

Autonomy autonomyFromString(std::string_view s, bool* ok)
{
    if (ok)
        *ok = true;
    if (iequals(s, "suggest")) return Autonomy::Suggest;
    if (iequals(s, "write"))   return Autonomy::Write;
    if (iequals(s, "commit"))  return Autonomy::Commit;
    if (iequals(s, "push"))    return Autonomy::Push;
    if (iequals(s, "full"))    return Autonomy::Full;
    if (ok)
        *ok = false;
    return Autonomy::Commit;
}

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
    if (iequals(s, "quit"))   return OnChildExit::Quit;
    if (iequals(s, "ask"))    return OnChildExit::Ask;
    if (iequals(s, "return")) return OnChildExit::Return;
    if (ok)
        *ok = false;
    return OnChildExit::Return;
}

// -------------------------------------------------------------------- config

Config Config::defaults()
{
    Config c;

    const fs::path profile = knownFolder(FOLDERID_Profile);
    c.root      = profile / "projects";
    c.claudeExe = profile / ".local" / "bin" / "claude.exe";

    c.autonomy = Autonomy::Commit;
    c.effort   = "max";

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

    const fs::path  path = filePath();
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

    const auto bad = [&](const std::string& msg) {
        if (status)
            *status = ConfigStatus::Unreadable;
        if (detail)
            *detail = path.string() + ": " + msg;
        return Config::defaults();
    };

    if (const auto v = str("root"))    c.root   = fs::path(widen(*v));
    if (const auto v = str("git_exe")) c.gitExe = fs::path(widen(*v));

    if (const auto v = str("autonomy")) {
        bool ok = false;
        c.autonomy = autonomyFromString(*v, &ok);
        if (!ok)
            return bad("autonomy must be suggest, write, commit, push or full");
    }

    if (const auto v = str("launch", "claude"))        c.claudeExe    = fs::path(widen(*v));
    if (const auto v = str("launch", "model"))         c.model        = *v;
    if (const auto v = str("launch", "effort"))        c.effort       = *v;
    if (const auto v = str("launch", "terminal"))      c.terminalExe  = fs::path(widen(*v));
    if (const auto v = str("launch", "terminal_args")) c.terminalArgs = *v;

    if (tbl["launch"]["skip_permissions"].is_boolean()) {
        c.skipPermissions         = tbl["launch"]["skip_permissions"].value_or(true);
        c.skipPermissionsExplicit = true;
    }

    if (tbl["launch"]["extra_args"].is_array())
        c.extraArgs = stringArray(tbl["launch"]["extra_args"]);

    // Read after the structured keys, so an explicitly set launch.effort is not
    // clobbered by a stale launch.args carrying the same flag.
    if (tbl["launch"]["args"].is_array()) {
        Config migrated = c;
        migrateLaunchArgs(migrated, stringArray(tbl["launch"]["args"]));
        if (!str("launch", "effort"))
            c.effort = migrated.effort;
        if (!str("launch", "model"))
            c.model = migrated.model;
        if (!tbl["launch"]["skip_permissions"].is_boolean()) {
            c.skipPermissions         = migrated.skipPermissions;
            c.skipPermissionsExplicit = migrated.skipPermissionsExplicit;
        }
        if (!tbl["launch"]["extra_args"].is_array())
            c.extraArgs = migrated.extraArgs;
    }

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

    if (const auto v = str("dock", "exe"))
        c.dockExe = fs::path(widen(*v));
    c.dockStartTimeoutMs =
        tbl["dock"]["start_timeout_ms"].value_or(c.dockStartTimeoutMs);

    c.dispatchMaxRepos = tbl["dispatch"]["max_repos"].value_or(c.dispatchMaxRepos);
    c.dispatchMaxItems = tbl["dispatch"]["max_items"].value_or(c.dispatchMaxItems);

    // The two booleans the ladder replaced. An explicit autonomy wins, so this
    // only fires for a config written before the ladder existed.
    if (!str("autonomy")
        && (tbl["dispatch"]["commit"].is_boolean() || tbl["dispatch"]["push"].is_boolean())) {
        const bool commit = tbl["dispatch"]["commit"].value_or(true);
        const bool push   = tbl["dispatch"]["push"].value_or(false);
        c.autonomy = push ? Autonomy::Push : (commit ? Autonomy::Commit : Autonomy::Write);
    }

    if (const auto v = str("ui", "sort")) {
        if (!iequals(*v, "recent") && !iequals(*v, "name") && !iequals(*v, "dirty")
            && !iequals(*v, "open")) {
            return bad("ui.sort must be recent, name, dirty or open");
        }
        c.sort = toLower(*v);
    }
    if (const auto v = str("ui", "on_exit")) {
        bool ok = false;
        c.onExit = onChildExitFromString(*v, &ok);
        if (!ok)
            return bad("ui.on_exit must be return, quit or ask");
    }

    if (status)
        *status = ConfigStatus::Loaded;
    return c;
}

bool Config::save(std::string* error) const
{
    fs::path path = portablePath();
    std::error_code ec;
    if (path.empty() || !fs::exists(path, ec))
        path = perUserPath();

    if (path.empty()) {
        if (error)
            *error = "cannot resolve %LOCALAPPDATA%";
        return false;
    }

    fs::create_directories(path.parent_path(), ec);

    // TOML literal strings for every path. In a basic string "{InstallDir}\bin"
    // parses \b as a backspace, which is the same trap Forge's config schema
    // documents for installer.toml.
    std::ostringstream os;
    os << "# ProjectMan configuration.\n"
       << "# A copy of this file beside the executable takes precedence over this one.\n"
       << "# Unknown keys are rejected rather than ignored.\n"
       << "#\n"
       << "# Edit here, or with `pm config set <key> <value>`, or in either app.\n\n";

    os << "root = '" << root.string() << "'\n\n";

    os << "# How far Claude Code may go on its own. Each rung contains the ones\n"
       << "# below it.\n";
    for (const Autonomy a : { Autonomy::Suggest, Autonomy::Write, Autonomy::Commit,
                              Autonomy::Push, Autonomy::Full }) {
        os << "#   " << autonomyName(a);
        os << std::string(std::max<size_t>(1, 9 - std::strlen(autonomyName(a))), ' ');
        os << autonomySummary(a) << "\n";
    }
    os << "autonomy = \"" << autonomyName(autonomy) << "\"\n\n";

    os << "[launch]\n";
    os << "claude = '" << claudeExe.string() << "'\n";
    if (!model.empty())
        os << "model  = \"" << model << "\"\n";
    os << "effort = \"" << effort << "\"\n";
    if (skipPermissionsExplicit) {
        os << "# Set explicitly, so it no longer follows the autonomy ladder.\n";
        os << "skip_permissions = " << (skipPermissions ? "true" : "false") << "\n";
    }
    os << "extra_args    = [";
    for (size_t i = 0; i < extraArgs.size(); ++i)
        os << (i ? ", " : "") << '"' << extraArgs[i] << '"';
    os << "]\n";
    os << "terminal_args = \"" << terminalArgs << "\"\n\n";

    os << "[scan]\n";
    os << "threads            = " << scanThreads << "   # 0 picks 3/4 of the CPUs\n";
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

    os << "[dock]\n";
    if (!dockExe.empty())
        os << "exe = '" << dockExe.string() << "'\n";
    os << "start_timeout_ms = " << dockStartTimeoutMs << "\n\n";

    os << "[dispatch]\n";
    os << "max_repos = " << dispatchMaxRepos << "\n";
    os << "max_items = " << dispatchMaxItems << "\n\n";

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

// ------------------------------------------------------------------ settings

const std::vector<Setting>& settings()
{
    static const std::vector<Setting> s = {
        { "root", "Projects root",
          "The directory ProjectMan indexes.", SettingKind::Path, nullptr, 0, 0 },

        { "autonomy", "Autonomy",
          "How far Claude Code may go on its own.", SettingKind::Choice,
          "suggest,write,commit,push,full", 0, 0 },

        { "launch.effort", "Effort",
          "Reasoning effort for every session.", SettingKind::Choice,
          "low,medium,high,xhigh,max", 0, 0 },

        { "launch.model", "Model",
          "Model alias, or blank for the default.", SettingKind::Choice,
          ",fable,sonnet,opus", 0, 0 },

        { "launch.skip_permissions", "Skip permissions",
          "Follows autonomy unless set here.", SettingKind::Bool, nullptr, 0, 0 },

        { "launch.claude", "Claude Code",
          "Path to claude.exe.", SettingKind::Path, nullptr, 0, 0 },

        { "launch.terminal_args", "Terminal args",
          "Passed to wt.exe before the working directory.", SettingKind::Text,
          nullptr, 0, 0 },

        { "ui.on_exit", "When Claude Code exits",
          "Console only: return to the list, or quit.", SettingKind::Choice,
          "return,quit,ask", 0, 0 },

        { "ui.sort", "Sort by",
          "Default order for the list.", SettingKind::Choice,
          "recent,name,dirty,open", 0, 0 },

        { "dock.exe", "Docked Console",
          "Path to dockedconsole.exe. Blank finds it.", SettingKind::Path,
          nullptr, 0, 0 },

        { "dock.start_timeout_ms", "Dock start wait",
          "Milliseconds to wait for a cold dock, which may sit behind a UAC prompt.",
          SettingKind::Int, nullptr, 5000, 180000 },

        { "dispatch.max_repos", "Dispatch repo cap",
          "How many repositories a dispatch preselects.", SettingKind::Int,
          nullptr, 1, 64 },

        { "dispatch.max_items", "Dispatch item cap",
          "How many items a briefing may carry.", SettingKind::Int, nullptr, 1, 500 },

        { "scan.threads", "Scan threads",
          "0 picks three quarters of the CPUs.", SettingKind::Int, nullptr, 0, 64 },

        { "scan.include_plain", "Show folders",
          "List directories that are not git repositories.", SettingKind::Bool,
          nullptr, 0, 0 },

        { "scan.descend_containers", "Descend containers",
          "Find repositories one level inside a plain directory.",
          SettingKind::Bool, nullptr, 0, 0 },

        { "scan.exclude", "Exclude",
          "Directory names never indexed.", SettingKind::StringList, nullptr, 0, 0 },

        { "github.enabled", "GitHub",
          "Fetch open pull requests and issues with gh.", SettingKind::Bool,
          nullptr, 0, 0 },

        { "github.owners", "GitHub owners",
          "Accounts and organisations to search.", SettingKind::StringList,
          nullptr, 0, 0 },

        { "github.cache_minutes", "GitHub cache",
          "Minutes before a refetch.", SettingKind::Int, nullptr, 1, 10080 },
    };
    return s;
}

const Setting* findSetting(std::string_view key)
{
    for (const Setting& s : settings()) {
        if (iequals(s.key, key))
            return &s;
    }
    return nullptr;
}

std::string readSetting(const Config& cfg, std::string_view key)
{
    if (iequals(key, "root"))                     return cfg.root.string();
    if (iequals(key, "autonomy"))                 return autonomyName(cfg.autonomy);
    if (iequals(key, "git_exe"))                  return cfg.gitExe.string();
    if (iequals(key, "launch.claude"))            return cfg.claudeExe.string();
    if (iequals(key, "launch.model"))             return cfg.model;
    if (iequals(key, "launch.effort"))            return cfg.effort;
    if (iequals(key, "launch.terminal_args"))     return cfg.terminalArgs;
    if (iequals(key, "launch.extra_args"))        return joinList(cfg.extraArgs);
    if (iequals(key, "launch.skip_permissions")) {
        return cfg.skipPermissionsExplicit
                 ? (cfg.skipPermissions ? "true" : "false")
                 : std::string(cfg.resolvedSkipPermissions() ? "true" : "false")
                       + " (from autonomy)";
    }
    if (iequals(key, "scan.threads"))             return std::to_string(cfg.scanThreads);
    if (iequals(key, "scan.timeout_ms"))          return std::to_string(cfg.probeTimeoutMs);
    if (iequals(key, "scan.include_plain"))       return cfg.includePlain ? "true" : "false";
    if (iequals(key, "scan.descend_containers"))  return cfg.descendContainers ? "true" : "false";
    if (iequals(key, "scan.exclude"))             return joinList(cfg.exclude);
    if (iequals(key, "github.enabled"))           return cfg.githubEnabled ? "true" : "false";
    if (iequals(key, "github.owners"))            return joinList(cfg.githubOwners);
    if (iequals(key, "github.cache_minutes"))     return std::to_string(cfg.githubCacheMinutes);
    if (iequals(key, "dock.exe"))                 return cfg.dockExe.string();
    if (iequals(key, "dock.start_timeout_ms"))    return std::to_string(cfg.dockStartTimeoutMs);
    if (iequals(key, "dispatch.max_repos"))       return std::to_string(cfg.dispatchMaxRepos);
    if (iequals(key, "dispatch.max_items"))       return std::to_string(cfg.dispatchMaxItems);
    if (iequals(key, "ui.sort"))                  return cfg.sort;
    if (iequals(key, "ui.on_exit"))               return toString(cfg.onExit);
    return {};
}

bool applySetting(Config& cfg, std::string_view key, std::string_view value,
                  std::string* error)
{
    const Setting* s = findSetting(key);
    if (!s && !iequals(key, "git_exe") && !iequals(key, "launch.extra_args")
        && !iequals(key, "scan.timeout_ms")) {
        if (error)
            *error = "unknown setting \"" + std::string(key) + "\"";
        return false;
    }

    const auto fail = [&](const std::string& msg) {
        if (error)
            *error = msg;
        return false;
    };

    const auto asInt = [&](int lo, int hi, int* out) {
        const std::string v(trim(value));
        if (v.empty() || v.find_first_not_of("-0123456789") != std::string::npos)
            return false;
        const long n = std::atol(v.c_str());
        if (n < lo || n > hi)
            return false;
        *out = static_cast<int>(n);
        return true;
    };

    if (iequals(key, "root")) {
        cfg.root = fs::path(widen(trim(value)));
        return true;
    }
    if (iequals(key, "autonomy")) {
        bool ok = false;
        const Autonomy a = autonomyFromString(trim(value), &ok);
        if (!ok)
            return fail("autonomy must be suggest, write, commit, push or full");
        cfg.autonomy = a;
        return true;
    }
    if (iequals(key, "git_exe")) {
        cfg.gitExe = fs::path(widen(trim(value)));
        return true;
    }
    if (iequals(key, "launch.claude")) {
        cfg.claudeExe = fs::path(widen(trim(value)));
        return true;
    }
    if (iequals(key, "launch.model")) {
        const std::string v(trim(value));
        if (!v.empty() && !iequals(v, "fable") && !iequals(v, "sonnet")
            && !iequals(v, "opus")) {
            return fail("model must be fable, sonnet, opus, or blank");
        }
        cfg.model = toLower(v);
        return true;
    }
    if (iequals(key, "launch.effort")) {
        const std::string v = toLower(trim(value));
        if (v != "low" && v != "medium" && v != "high" && v != "xhigh" && v != "max")
            return fail("effort must be low, medium, high, xhigh or max");
        cfg.effort = v;
        return true;
    }
    if (iequals(key, "launch.skip_permissions")) {
        bool b = false;
        if (!parseBool(trim(value), &b))
            return fail("expected true or false");
        cfg.skipPermissions         = b;
        cfg.skipPermissionsExplicit = true;
        return true;
    }
    if (iequals(key, "launch.terminal_args")) {
        cfg.terminalArgs = std::string(trim(value));
        return true;
    }
    if (iequals(key, "launch.extra_args")) {
        cfg.extraArgs = splitList(value);
        return true;
    }
    if (iequals(key, "scan.threads"))
        return asInt(0, 64, &cfg.scanThreads) || fail("expected 0 to 64");
    if (iequals(key, "scan.timeout_ms"))
        return asInt(1000, 600000, &cfg.probeTimeoutMs) || fail("expected 1000 to 600000");
    if (iequals(key, "scan.include_plain")) {
        bool b = false;
        if (!parseBool(trim(value), &b))
            return fail("expected true or false");
        cfg.includePlain = b;
        return true;
    }
    if (iequals(key, "scan.descend_containers")) {
        bool b = false;
        if (!parseBool(trim(value), &b))
            return fail("expected true or false");
        cfg.descendContainers = b;
        return true;
    }
    if (iequals(key, "scan.exclude")) {
        cfg.exclude = splitList(value);
        return true;
    }
    if (iequals(key, "github.enabled")) {
        bool b = false;
        if (!parseBool(trim(value), &b))
            return fail("expected true or false");
        cfg.githubEnabled = b;
        return true;
    }
    if (iequals(key, "github.owners")) {
        cfg.githubOwners = splitList(value);
        return true;
    }
    if (iequals(key, "github.cache_minutes"))
        return asInt(1, 10080, &cfg.githubCacheMinutes) || fail("expected 1 to 10080");
    if (iequals(key, "dock.exe")) {
        cfg.dockExe = fs::path(widen(trim(value)));
        return true;
    }
    if (iequals(key, "dock.start_timeout_ms"))
        return asInt(5000, 180000, &cfg.dockStartTimeoutMs)
            || fail("expected 5000 to 180000");
    if (iequals(key, "dispatch.max_repos"))
        return asInt(1, 64, &cfg.dispatchMaxRepos) || fail("expected 1 to 64");
    if (iequals(key, "dispatch.max_items"))
        return asInt(1, 500, &cfg.dispatchMaxItems) || fail("expected 1 to 500");
    if (iequals(key, "ui.sort")) {
        const std::string v = toLower(trim(value));
        if (v != "recent" && v != "name" && v != "dirty" && v != "open")
            return fail("sort must be recent, name, dirty or open");
        cfg.sort = v;
        return true;
    }
    if (iequals(key, "ui.on_exit")) {
        bool ok = false;
        cfg.onExit = onChildExitFromString(trim(value), &ok);
        return ok || fail("expected return, quit or ask");
    }

    return fail("unknown setting \"" + std::string(key) + "\"");
}

std::string cycleSetting(const Config& cfg, std::string_view key, int direction)
{
    const Setting* s = findSetting(key);
    if (!s)
        return {};

    if (s->kind == SettingKind::Bool)
        return iequals(readSetting(cfg, key).substr(0, 4), "true") ? "false" : "true";

    if (s->kind == SettingKind::Int) {
        int  n = std::atoi(readSetting(cfg, key).c_str());
        // Coarse steps on the wide ranges, so a cache TTL is not 10,080 presses
        // from one end to the other.
        const int step = (s->max - s->min) > 200 ? 30 : 1;
        n = std::clamp(n + direction * step, s->min, s->max);
        return std::to_string(n);
    }

    if (s->kind == SettingKind::Choice && s->choices) {
        const std::vector<std::string> opts = splitList(s->choices);
        // splitList drops empties, so a leading comma meaning "blank" has to be
        // put back for launch.model.
        std::vector<std::string> all;
        if (s->choices[0] == ',')
            all.push_back("");
        all.insert(all.end(), opts.begin(), opts.end());
        if (all.empty())
            return {};

        const std::string cur = readSetting(cfg, key);
        int               idx = 0;
        for (size_t i = 0; i < all.size(); ++i) {
            if (iequals(all[i], cur)) {
                idx = static_cast<int>(i);
                break;
            }
        }
        const int n = static_cast<int>(all.size());
        idx = ((idx + direction) % n + n) % n;
        return all[static_cast<size_t>(idx)];
    }

    return {};
}

} // namespace pm
