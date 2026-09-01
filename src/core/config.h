#pragma once

#include "model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pm {

enum class ConfigStatus {
    Loaded,
    Missing,
    Unreadable,
};

// What the console does once Claude Code exits.
enum class OnChildExit {
    Return,   // re-enter the picker
    Quit,     // exit, propagating the child's code
    Ask,
};

// How far Claude Code is allowed to go on its own.
//
// One ladder, and every rung contains the ones below it. It governs two things:
// which flags a launch carries, and what the dispatch briefing permits. Keeping
// them on one setting is the point. A briefing that says "commit your work"
// while the session runs without permission to write is a contradiction the
// user would have to notice themselves.
enum class Autonomy {
    Suggest,   // read and report; changes nothing, and asks before every tool
    Write,     // edit the working tree, do not commit
    Commit,    // edit and commit, but nothing leaves the machine
    Push,      // commit and push a branch
    Full,      // push and open pull requests
};

const char* autonomyName(Autonomy a);       // "commit"
const char* autonomyLabel(Autonomy a);      // "COMMIT"
const char* autonomySummary(Autonomy a);    // one line, for a settings surface
Autonomy    autonomyFromString(std::string_view s, bool* ok = nullptr);

inline bool mayWrite(Autonomy a)  { return a >= Autonomy::Write; }
inline bool mayCommit(Autonomy a) { return a >= Autonomy::Commit; }
inline bool mayPush(Autonomy a)   { return a >= Autonomy::Push; }
inline bool mayOpenPr(Autonomy a) { return a >= Autonomy::Full; }

struct Config {
    fs::path root;

    // The headline setting, so it sits at the top level rather than inside a
    // table nobody reads.
    Autonomy autonomy = Autonomy::Commit;

    // Launch
    fs::path    claudeExe;
    std::string model;    // --model alias; empty leaves Claude Code's default
    std::string effort;   // --effort level; empty leaves the default

    // Whether to pass --dangerously-skip-permissions. Follows the autonomy
    // ladder unless it is set explicitly: asking permission for every edit is
    // the whole of Suggest, and answering a prompt per edit defeats the rest.
    bool skipPermissions        = true;
    bool skipPermissionsExplicit = false;

    std::vector<std::string> extraArgs;   // appended to every launch

    fs::path    terminalExe;   // empty resolves wt.exe
    std::string terminalArgs = "-w new";

    // Dock
    fs::path dockExe;                    // empty resolves dockedconsole.exe
    int      dockStartTimeoutMs = 45000; // a cold dock may sit behind a UAC prompt

    // Whether an ordinary launch goes to a Docked Console column. Auto means
    // it does whenever Docked Console can be found, which is the point: the
    // dock is where terminals live, so nothing should have to ask for it.
    // Holding Shift over a launch forces a loose window for that one launch.
    bool dockAuto = true;

    // Scan
    int                      scanThreads       = 0;   // 0 = defaultScanThreads()
    int                      probeTimeoutMs    = 20000;
    bool                     descendContainers = true;
    bool                     includePlain      = true;
    std::vector<std::string> exclude;

    // GitHub
    bool                     githubEnabled = true;
    std::vector<std::string> githubOwners;
    int                      githubCacheMinutes = 60;

    // Dispatch
    int dispatchMaxRepos = 8;
    int dispatchMaxItems = 40;

    // UI
    std::string sort   = "recent";   // recent | name | dirty | open
    OnChildExit onExit = OnChildExit::Return;

    fs::path gitExe;   // empty resolves from PATH

    // Resolved once the ladder and any explicit override are both known.
    bool resolvedSkipPermissions() const
    {
        return skipPermissionsExplicit ? skipPermissions : mayWrite(autonomy);
    }

    // A copy beside the executable wins over the per-user one, so a portable
    // checkout can carry its own settings.
    static fs::path filePath();
    static fs::path perUserPath();
    static fs::path portablePath();

    static Config load(ConfigStatus* status = nullptr, std::string* detail = nullptr);

    // Writing is always safe to call. Loading only writes back on Missing: an
    // Unreadable file is never rewritten, because it is the only copy of
    // something a person typed and a stray comma should not cost them their
    // settings.
    bool save(std::string* error = nullptr) const;

    fs::path resolveTerminal() const;
    fs::path resolveClaude() const;

    static Config defaults();
};

std::string toString(OnChildExit e);
OnChildExit onChildExitFromString(std::string_view s, bool* ok = nullptr);

// ---------------------------------------------------------------- settings
//
// One description of the editable surface, so the console and the desktop
// front ends offer the same settings in the same order with the same wording,
// and `pm config set` cannot drift from either.

enum class SettingKind {
    Text,
    Path,
    Bool,
    Int,
    Choice,
    StringList,
};

struct Setting {
    const char* key;      // "autonomy", "launch.effort"
    const char* label;    // "Autonomy"
    const char* help;     // one line
    SettingKind kind;
    const char* choices;  // Choice only: comma-separated
    int         min, max; // Int only
};

const std::vector<Setting>& settings();
const Setting*              findSetting(std::string_view key);

// Both return false with a reason when the value does not fit the setting.
bool        applySetting(Config& cfg, std::string_view key, std::string_view value,
                         std::string* error);
std::string readSetting(const Config& cfg, std::string_view key);

// The next or previous value for a setting that cycles, which is what a
// left/right key press in the console settings view needs.
std::string cycleSetting(const Config& cfg, std::string_view key, int direction);

} // namespace pm
