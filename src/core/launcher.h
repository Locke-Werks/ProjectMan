#pragma once

#include "config.h"
#include "model.h"
#include "workitems.h"

#include <string>
#include <vector>

namespace pm {

enum class LaunchMode {
    New,        // bare claude
    Continue,   // -c, the most recent conversation in this directory
    Resume,     // -r, a specific session or the picker
};

struct LaunchSpec {
    fs::path                 cwd;
    LaunchMode               mode = LaunchMode::New;
    std::string              resumeId;    // optional, with Resume
    std::string              model;       // overrides Config::model
    std::string              prompt;      // seeded initial prompt, optional
    std::vector<fs::path>    addDirs;     // --add-dir, for a multi-repo dispatch
    std::vector<std::string> extra;
};

// Build the argv for claude.exe. Pure, so it is testable without launching.
std::vector<std::string> claudeArgs(const LaunchSpec& s, const Config& cfg);

struct HandoffResult {
    bool        started  = false;
    int         exitCode = 0;
    std::string error;
};

// True while a child owns the console. The console control handler reads this
// to decide whether a Ctrl+C belongs to the child.
bool childLive();

// Hand THIS terminal to Claude Code and block until it exits.
//
// The caller must have fully restored the console (left the alternate buffer,
// restored the saved input and output modes) before calling. Nothing here
// creates a console or a process group: the child inherits both, which is the
// entire point.
HandoffResult handoff(const LaunchSpec& s, const Config& cfg);

// Open a new Windows Terminal window running Claude Code in the project.
//
// Returns as soon as the wt stub exits, which is immediately: wt hands off to
// an already-running WindowsTerminal monarch and dies. There is nothing to wait
// on and the stub's exit says nothing about the session.
bool openInTerminal(const LaunchSpec& s, const Config& cfg, std::string* error);

// A terminal with nothing attached, in the project directory.
//
// Separate because openInTerminal always appends claude.exe: through 0.1.0 the
// TERMINAL button ran Claude Code exactly like ENGAGE, under a comment claiming
// it did not.
bool openShellInTerminal(const fs::path& cwd, const Config& cfg, std::string* error);

// Turn a dispatch plan into a launch: cwd is the projects root, every selected
// repository is passed with --add-dir, and the briefing becomes the prompt.
LaunchSpec dispatchSpec(const DispatchPlan& plan, const Config& cfg);

} // namespace pm
