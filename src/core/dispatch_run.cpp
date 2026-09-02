#include "dispatch_run.h"

#include <mutex>
#include <system_error>

namespace pm {
namespace {

// Lines in, events out, with the running tally kept alongside. Two reader
// threads feed this, so the tally is behind a lock.
class EventAdapter : public LineSink {
public:
    EventAdapter(RunSink& sink, claude::RunStats& stats) : sink_(sink), stats_(stats) {}

    void onLine(Stream which, std::string line) override
    {
        if (which == Stream::Err) {
            deliver(claude::stderrEvent(std::move(line)));
            return;
        }
        for (const claude::Event& e : claude::parseLine(line))
            deliver(e);
    }

private:
    void deliver(const claude::Event& e)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            claude::account(stats_, e);
        }
        sink_.onEvent(e);
    }

    RunSink&          sink_;
    claude::RunStats& stats_;
    std::mutex        mutex_;
};

} // namespace

RunOutcome runDispatch(const DispatchPlan& plan, const Config& cfg, CancelToken& token,
                       RunSink& sink)
{
    RunOutcome out;

    const fs::path claude = cfg.resolveClaude();
    std::error_code ec;
    if (claude.empty() || !fs::exists(claude, ec)) {
        out.proc.launchError = "claude.exe not found at " + claude.string();
        out.summary          = claude::summarize(out.stats, out.proc);
        return out;
    }

    const LaunchSpec spec = dispatchSpec(plan, cfg);

    StreamSpec s;
    s.exe       = claude;
    s.args      = claudePrintArgs(spec, cfg);
    s.cwd       = spec.cwd;
    s.stdinData = spec.prompt;

    EventAdapter adapter(sink, out.stats);
    out.proc    = runStreaming(s, token, adapter);
    out.summary = claude::summarize(out.stats, out.proc);
    return out;
}

LaunchSpec continueSpec(const DispatchPlan& plan, const Config& cfg,
                        const std::string& sessionId)
{
    LaunchSpec s = dispatchSpec(plan, cfg);
    s.prompt.clear();
    s.mode     = LaunchMode::Resume;
    s.resumeId = sessionId;
    return s;
}

} // namespace pm
