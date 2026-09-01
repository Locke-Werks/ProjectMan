#pragma once

#include "config.h"
#include "launcher.h"

#include <string>

namespace pm::dock {

// Why a dock launch did not happen. One enum, so the console, the desktop app
// and `pm dock` refuse in the same words.
enum class Status {
    Ok,
    NoClaude,       // claude.exe could not be resolved
    NotInstalled,   // dockedconsole.exe could not be resolved
    StartFailed,    // it was started and no dock window ever appeared
    NotHandled,     // a dock window exists and did not answer
    Full,           // every column is at its pane cap
    NoRoom,         // the display cannot take another column
    Unnamed,        // the dock has window naming turned off
    Rejected,       // the dock refused the request as malformed
};

// One line, already phrased for a person. No full stop, so a caller can append
// a detail.
const char* statusText(Status s);

// Is a dock running right now, and how many columns does it have?
//
// Columns are counted by enumerating the host window's DockedConsole.Column
// children. Nothing needs reading out of them: the dock decides where a pane
// goes and says so in its reply, so this is only for `pm doctor`.
struct State {
    bool          running  = false;
    unsigned long pid      = 0;
    int           columns  = 0;
    bool          elevated = false;
};
State discover();

// dock.exe from the config, then the ARP InstallLocation, then Program Files,
// then PATH. Docked Console's installer writes only a Start Menu shortcut and
// an ARP entry, so it is never on PATH after an ordinary install.
fs::path resolveExe(const Config& cfg);

// Where Docked Console comes from when it is not installed. GitHub redirects
// both releases/latest forms to the newest release, so neither needs revisiting
// when a version ships. The repository is public, so no sign-in stands in the
// way of the download.
const char* releasePage();
const char* installerUrl();

// Hands one of those to the shell, which opens it in whatever the user has set
// as their browser. False means the shell refused, with the reason in error.
bool openDownload(const char* url, std::string* error);

// The whole DOCK action: find or start the dock, then ask it for a pane
// running Claude Code in this project. Never falls back to a window of its own.
//
// The dock runs the wt command, not this process. Windows Terminal keeps
// elevated and unelevated windows in separate monarch worlds, and aiming wt at
// the wrong one does not fail, it makes a new window that permanently steals
// the name. See the dock's README.
Status launch(const LaunchSpec& s, const Config& cfg, std::string* detail);

} // namespace pm::dock
