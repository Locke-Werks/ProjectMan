#pragma once

#include "model.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace pm::git {

namespace fs = std::filesystem;

// Located once: an explicit config override, then PATH, then the usual install
// directories. Empty when git is not installed.
const fs::path& exePath();
void setExePathOverride(const fs::path& p);

struct RepoProbe {
    bool     isRepo = false;
    bool     isBare = false;
    fs::path gitDir;   // absolute, already resolved through a .git file
};

// Cheap classification with no process at all: .git exists as either a file or
// a directory (the file form is a linked worktree), or the directory itself
// looks bare. rev-parse stays for the probe, where a process is spawned anyway.
ProjectKind classify(const fs::path& dir);

// One spawn, two answers.
RepoProbe probe(const fs::path& dir);

// Pure, so it can be tested against captured fixtures without running git.
GitStatus parsePorcelainV2(std::string_view text);

struct ProbeOptions {
    int  timeoutMs      = 20000;
    bool wantLastCommit = true;
};

// One or two git invocations for a single project, synchronously, on the
// calling thread. Safe to call concurrently.
GitStatus status(const fs::path& workdir, const RepoProbe& probe,
                 const ProbeOptions& opt = {});

// Read from <gitdir>/config rather than spawning `git config`, which saves one
// process per repo across the whole sweep. Falls back to git when the file
// yields nothing, since conditional includes can put the remote elsewhere.
std::string originUrl(const fs::path& workdir, const fs::path& gitDir);

// "https://github.com/Locke-Werks/Forge.git" -> "Locke-Werks/Forge"
// "git@github.com:nyxlocke/BodyLore.git"     -> "nyxlocke/BodyLore"
// Anything that is not a GitHub remote returns empty.
std::string ownerRepoFromUrl(std::string_view url);

// Linked worktrees, counted from <gitdir>/worktrees. Zero processes.
int countWorktrees(const fs::path& gitDir);

} // namespace pm::git
