#pragma once

#include "config.h"
#include "model.h"

namespace pm {

// The slower half of "open items". Git state is cheap and runs in the sweep;
// these three are either I/O heavy or network bound, so they run afterwards and
// never block the first paint.

// ~/.claude/history.jsonl carries an unmangled absolute project path on every
// record, so this is a single forward pass with no slug inversion. Fills
// claudePrompts and claudeLastUnix, then tails the newest transcript in
// ~/.claude/projects/<slug>/ for its ai-title and last-prompt records.
//
// Note there is no todo state to read: ~/.claude/todos does not exist and
// TodoWrite has never persisted anything on this machine.
void enrichClaudeState(ProjectList& projects, const Config& cfg);

// Unchecked "- [ ]" in markdown, plus spec-kit specs/<nnn>-<name>/spec.md.
// The exclude set is load-bearing rather than tidy: without .claude pruned,
// one fork's agent templates contribute 45 files and dominate every count.
void enrichChecklists(ProjectList& projects, const Config& cfg);

struct GitHubTotals {
    int repos  = 0;
    int prs    = 0;
    int issues = 0;
};

// Open pull requests and issues, in two `gh search` calls for the whole owner
// set rather than one `gh issue list` per repository. Cached to
// %LOCALAPPDATA%\ProjectMan\github.tsv with a TTL.
//
// Two calls, not one: `gh search issues` returns only issues, and every result
// reports isPullRequest false, so pull requests need `gh search prs`.
//
// Repos are keyed by ownerRepo, which comes from the origin URL: the folder
// name is wrong often enough to matter (AAP is apogee-asset-pipeline, MTGSim
// is tolaria, MilkDrop3 is milkrun).
//
// `totals` reports distinct counts from the fetch itself. Summing the projects
// instead would double-count, because several repositories are checked out in
// more than one place under this tree.
bool enrichGitHub(ProjectList& projects, const Config& cfg, bool force,
                  GitHubTotals* totals, std::string* error);

fs::path githubCachePath();

} // namespace pm
