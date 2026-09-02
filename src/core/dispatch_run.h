#pragma once

#include "claude_stream.h"
#include "config.h"
#include "launcher.h"
#include "model.h"
#include "proc.h"
#include "workitems.h"

namespace pm {

// Where a dispatch run's events go. Called FROM A READER THREAD, so an
// implementation posts to its own thread rather than touching a widget.
class RunSink {
public:
    virtual ~RunSink() = default;
    virtual void onEvent(const claude::Event& e) = 0;
};

struct RunOutcome {
    StreamResult       proc;
    claude::RunStats   stats;
    claude::RunSummary summary;
};

// Run the plan to completion with `claude -p`: cwd is the projects root, every
// selected repository is an --add-dir, and the briefing goes in on stdin.
// Blocks; raise the token to stop it.
RunOutcome runDispatch(const DispatchPlan& plan, const Config& cfg, CancelToken& token,
                       RunSink& sink);

// The launch that picks a finished run up again interactively: the same cwd and
// --add-dir set, resuming the session the run left behind.
LaunchSpec continueSpec(const DispatchPlan& plan, const Config& cfg,
                        const std::string& sessionId);

} // namespace pm
