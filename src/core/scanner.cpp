#include "scanner.h"

#include <algorithm>
#include <chrono>

namespace pm {

void deriveOpenItems(Project& p)
{
    const GitStatus& g = p.git;
    p.open.uncommitted = g.dirtyCount();
    p.open.unpushed    = g.ahead;
    p.open.unpulled    = g.behind;
    p.open.stashes     = g.stashes;
}

void probeProject(Project& p, const git::ProbeOptions& opt)
{
    if (p.kind == ProjectKind::BareRepo) {
        p.git.state  = GitState::Bare;
        p.git.branch = "(bare)";
        return;
    }

    if (p.kind != ProjectKind::Repo) {
        p.git.state = GitState::NotARepo;
        return;
    }

    const git::RepoProbe rp = git::probe(p.path);
    p.git                   = git::status(p.path, rp, opt);

    if (!rp.gitDir.empty()) {
        p.originUrl = git::originUrl(p.path, rp.gitDir);
        p.ownerRepo = git::ownerRepoFromUrl(p.originUrl);
    }

    deriveOpenItems(p);
}

int defaultScanThreads()
{
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0)
        return 8;
    const int n = static_cast<int>(hw * 3 / 4);
    return std::clamp(n, 4, 32);
}

ScanPool::ScanPool(int threads, git::ProbeOptions opt)
    : threads_(threads > 0 ? threads : defaultScanThreads()), opt_(opt)
{
}

ScanPool::~ScanPool() { joinAll(); }

bool ScanPool::running() const { return active_.load(std::memory_order_acquire) > 0; }

void ScanPool::joinAll()
{
    for (std::thread& t : workers_) {
        if (t.joinable())
            t.join();
    }
    workers_.clear();
}

void ScanPool::start(ProjectList& projects, StatusSink& sink, CancelToken& token)
{
    joinAll();

    next_.store(0, std::memory_order_relaxed);
    probed_.store(0, std::memory_order_relaxed);
    failed_.store(0, std::memory_order_relaxed);

    const int n = std::min<int>(threads_, static_cast<int>(projects.size()));
    if (n <= 0) {
        sink.onScanFinished(0, 0, 0.0);
        return;
    }

    const auto began = std::chrono::steady_clock::now();
    active_.store(n, std::memory_order_release);

    workers_.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        workers_.emplace_back([this, &projects, &sink, &token, began] {
            for (;;) {
                if (token.stop_requested())
                    break;

                const std::size_t idx = next_.fetch_add(1, std::memory_order_relaxed);
                if (idx >= projects.size())
                    break;

                Project& p = projects[idx];
                probeProject(p, opt_);

                if (p.git.state == GitState::Error)
                    failed_.fetch_add(1, std::memory_order_relaxed);
                if (p.kind == ProjectKind::Repo)
                    probed_.fetch_add(1, std::memory_order_relaxed);

                // Worker thread. The sink copies and marshals; it must not
                // block and must not touch any UI directly.
                sink.onStatus(idx, p);
            }

            // The last worker out reports, so the callback fires exactly once
            // and only after every result has been delivered.
            if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                const std::chrono::duration<double> secs =
                    std::chrono::steady_clock::now() - began;
                sink.onScanFinished(probed_.load(std::memory_order_relaxed),
                                    failed_.load(std::memory_order_relaxed),
                                    secs.count());
            }
        });
    }
}

void ScanPool::wait() { joinAll(); }

} // namespace pm
