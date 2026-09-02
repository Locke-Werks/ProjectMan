#include "dispatch_console.h"

#include "claude_stream.h"
#include "console.h"
#include "dispatch_run.h"

namespace pm::cli {
namespace {

// Prints what the shared renderer says to print. Two reader threads call this;
// printLine serialises them.
class ConsoleSink : public RunSink {
public:
    void onEvent(const claude::Event& e) override
    {
        if (const auto line = claude::render(e))
            printLine(line->text);
    }
};

} // namespace

int runDispatchOnConsole(const DispatchPlan& plan, const Config& cfg)
{
    for (const std::string& line : claude::describeRun(plan, cfg))
        printLine(line);
    printLine("");

    CancelToken token;
    installInterruptHandler();
    setInterruptTarget(&token);

    ConsoleSink      sink;
    const RunOutcome out = runDispatch(plan, cfg, token, sink);

    setInterruptTarget(nullptr);

    printLine("");
    printLine(out.summary.headline);

    if (!out.proc.started)
        return 4;
    if (out.proc.stopped)
        return 1;
    return out.proc.exitCode;
}

} // namespace pm::cli
