#pragma once

#include "config.h"
#include "console.h"
#include "model.h"
#include "workitems.h"

namespace pm::cli {

// What the browser wants to happen after it returns. The terminal handoff
// cannot run from inside the render loop: the console has to be fully restored
// first, so the loop exits with an intent and main carries it out.
enum class Action {
    Quit,
    LaunchNew,
    LaunchContinue,
    LaunchResume,
    OpenTerminal,
    OpenDock,
    Dispatch,
};

struct BrowseResult {
    Action       action = Action::Quit;
    const Project* project = nullptr;   // for the Launch* and OpenTerminal actions
    DispatchPlan plan;                  // for Dispatch
};

// Run the interactive picker. Returns when the user chooses an action or quits.
// The caller owns the console session and re-enters it if it wants the browser
// back after a child exits.
//
// cfg is mutable because the settings view edits it in place and writes it out.
// The caller's copy is the live one, so a change made here takes effect on the
// next launch without a restart.
BrowseResult browse(ConsoleSession& con, ProjectList& projects, Config& cfg);

} // namespace pm::cli
