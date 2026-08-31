#pragma once

#include "git.h"
#include "model.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace pm {

// Fill one project's git status and the git-derived half of its open items.
// Blocking, on the calling thread. Safe to call concurrently.
void probeProject(Project& p, const git::ProbeOptions& opt);

// Recompute the git-derived counters. Leaves the GitHub, checklist and Claude
// fields alone, since those are filled by slower passes that run separately.
void deriveOpenItems(Project& p);

// Three quarters of the logical CPUs, clamped to [4, 32].
//
// Measured over 78 repositories on a 32-CPU machine: 4 threads took 3.64 s,
// 8 took 2.36 s, 16 took 2.18 s, 24 took 1.69 s and 32 took 1.89 s. The cost
// is dominated by process spawn latency rather than by disk, so concurrency
// well past the "one thread per core is plenty" instinct keeps paying until it
// oversubscribes. Backing off a quarter lands on the floor of that curve and
// leaves the machine responsive while a sweep runs.
int defaultScanThreads();

class ScanPool {
public:
    explicit ScanPool(int threads = 0, git::ProbeOptions opt = {});
    ~ScanPool();

    ScanPool(const ScanPool&)            = delete;
    ScanPool& operator=(const ScanPool&) = delete;

    // Non-blocking. `projects` and `sink` must outlive the scan. The sink is
    // called from worker threads, once per project, in completion order.
    void start(ProjectList& projects, StatusSink& sink, CancelToken& token);

    void wait();
    bool running() const;

private:
    void joinAll();

    int                      threads_;
    git::ProbeOptions        opt_;
    std::vector<std::thread> workers_;
    std::atomic<std::size_t> next_{ 0 };
    std::atomic<int>         probed_{ 0 };
    std::atomic<int>         failed_{ 0 };
    std::atomic<int>         active_{ 0 };
};

} // namespace pm
