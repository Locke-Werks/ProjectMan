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
BrowseResult browse(ConsoleSession& con, ProjectList& projects, const Config& cfg);

} // namespace pm::cli
