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

} // namespace pm
