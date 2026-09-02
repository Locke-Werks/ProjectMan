#pragma once

#include "config.h"
#include "workitems.h"

namespace pm::cli {

// Runs the plan with `claude -p` and renders the stream on this console as
// plain text, line by line as it arrives, then the summary. Ctrl+C stops the
// run and pm survives to say so.
//
// Returns the child's exit code, 1 when the run was stopped, and 4 when it
// could not start at all (the launch error has already been printed).
int runDispatchOnConsole(const DispatchPlan& plan, const Config& cfg);

} // namespace pm::cli
