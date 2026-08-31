#pragma once

#include "model.h"

#include <string>
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

struct Config {
    fs::path root;

    // Launch
    fs::path                 claudeExe;
    std::vector<std::string> claudeArgs;   // applied to every launch
    std::string              model;        // --model alias; empty leaves the default
    fs::path                 terminalExe;  // empty resolves wt.exe
    std::string              terminalArgs = "-w new";

    // Scan
    int                      scanThreads     = 0;   // 0 = defaultScanThreads()
    int                      probeTimeoutMs  = 20000;
    bool                     descendContainers = true;
    bool                     includePlain      = true;
    std::vector<std::string> exclude;

    // GitHub
    bool                     githubEnabled = true;
    std::vector<std::string> githubOwners;
    int                      githubCacheMinutes = 60;

    // Dispatch: the multi-repo orchestrator.
    //
    // Push defaults off deliberately. An autonomous run that commits leaves
    // everything reviewable with git log and reversible with git reset; one
    // that pushes has already left the machine.
    bool dispatchCommit  = true;
    bool dispatchPush    = false;
    int  dispatchMaxRepos = 8;

    // UI
    std::string sort      = "recent";   // recent | name | dirty | open
    OnChildExit onExit    = OnChildExit::Return;

    fs::path gitExe;   // empty resolves from PATH

    // A copy beside the executable wins over the per-user one, so a portable
    // checkout can carry its own settings.
    static fs::path filePath();
    static fs::path perUserPath();
    static fs::path portablePath();

    static Config load(ConfigStatus* status = nullptr, std::string* detail = nullptr);

    // Called only when the file was Missing. An Unreadable file is never
    // rewritten: it is the only copy of something a person typed, and a stray
    // comma should not cost them their settings.
    bool save(std::string* error = nullptr) const;

    fs::path resolveTerminal() const;
    fs::path resolveClaude() const;

    static Config defaults();
};

std::string toString(OnChildExit e);
OnChildExit onChildExitFromString(std::string_view s, bool* ok = nullptr);

} // namespace pm
