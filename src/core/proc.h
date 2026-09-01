#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace pm {

namespace fs = std::filesystem;

struct ProcResult {
    int         exitCode = -1;
    std::string out;
    std::string err;
    bool        started  = false;   // false means CreateProcessW itself failed
    bool        timedOut = false;
    std::string launchError;        // human-readable reason when !started
    int         elapsedMs = 0;

    bool ok() const { return started && !timedOut && exitCode == 0; }
};

// Run a program to completion and capture both streams.
//
// Deliberately CreateProcessW and not QProcess: QProcess needs a running event
// loop to pump its readers, and core is Qt-free precisely so pm.exe can ship as
// a standalone console binary. std::system and popen are also wrong here, since
// both route through cmd.exe, which re-parses quoting and would mangle paths
// like "0 - ARCHON Design Language".
ProcResult run(const fs::path& exe,
               const std::vector<std::string>& args,
               const fs::path& cwd = {},
               int timeoutMs = 15000);

// Windows command-line quoting, per the rules CommandLineToArgvW parses back.
// Exposed because the launcher builds its own command lines.
std::string quoteArg(std::string_view arg);

// FormatMessageW for a GetLastError() value, trimmed to one line.
std::string errorText(unsigned long code);

// This process's environment, minus the markers that tell Claude Code it is a
// nested child of another session.
//
// ProjectMan is very often launched from inside a Claude Code session, and a
// plain inherited environment carries CLAUDE_CODE_CHILD_SESSION into every
// session it starts. Claude Code answers that marker by turning transcript
// saving off, which costs the new session its history, its --continue, and its
// place in ProjectMan's own list of unfinished work. A session ProjectMan
// launches is a new top-level session, so it gets a clean slate.
//
// Returns a double-NUL-terminated block for CreateProcessW, which needs
// CREATE_UNICODE_ENVIRONMENT alongside it.
std::vector<wchar_t> childEnvironment();

} // namespace pm
