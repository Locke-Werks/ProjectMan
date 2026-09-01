#pragma once

#include "config.h"
#include "model.h"

#include <string>
#include <vector>

namespace pm {

enum class WorkKind {
    Uncommitted,   // dirty working tree
    Unpushed,      // commits ahead of upstream
    Unpulled,      // commits behind upstream
    Stash,
    NoUpstream,    // a branch that has never been pushed anywhere
    OpenPr,
    OpenIssue,
    Checklist,     // unchecked "- [ ]" items in markdown
    Unfinished,    // a Claude Code session that stopped mid-thread
};

// One outstanding thing, in one project. This is the unit the dispatch view
// lists and the user ticks, so it has to read as a sentence on its own.
struct WorkItem {
    WorkKind    kind = WorkKind::Uncommitted;
    std::string project;      // display name
    std::string ownerRepo;    // "nyxlocke/BitsyGo", empty when there is no remote
    fs::path    path;         // the repo the work happens in
    std::string summary;      // "48 uncommitted files on master"
    std::string detail;       // extra context for the briefing; may be empty
    int         count = 0;
    bool        selected = false;

    // Rough ordering hint, high first. Drives which items are preselected.
    int weight() const;
};

using WorkList = std::vector<WorkItem>;

const char* kindLabel(WorkKind k);

// Derive every outstanding item across the scanned tree, ordered most
// significant first. Projects with nothing outstanding contribute nothing.
WorkList collectWorkItems(const ProjectList& projects);

// Preselect what is worth working without being asked: real, actionable, and
// unambiguous. Uncommitted trees and open PRs qualify; a branch merely being
// behind its upstream does not, since that is a pull, not work.
void preselect(WorkList& items, int maxRepos);

struct DispatchPlan {
    WorkList              items;    // only the selected ones
    std::vector<fs::path> repos;    // distinct, in first-appearance order
    std::string           briefing; // the prompt handed to Claude Code
};

struct DispatchOptions {
    Autonomy autonomy = Autonomy::Commit;
    int      maxRepos = 8;
    int      maxItems = 40;

    // Free text from the person dispatching, folded into the briefing above the
    // item list and given precedence over it. This is how a run gets scoped to
    // one job across the selected repositories ("rename every default branch to
    // main") rather than working the items as found. Empty for an ordinary run.
    std::string instructions;
};

// What a dispatch at this rung will and will not do, in one line, for a
// confirmation surface.
std::string dispatchSummary(Autonomy a);

// Build the plan from whatever the user ticked. Returns an empty plan when
// nothing is selected.
DispatchPlan buildDispatchPlan(const WorkList& items, const DispatchOptions& opt);

} // namespace pm
