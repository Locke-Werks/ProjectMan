#pragma once

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
    bool allowCommit = true;
    bool allowPush   = false;
    int  maxRepos    = 8;
};

// Build the plan from whatever the user ticked. Returns an empty plan when
// nothing is selected.
DispatchPlan buildDispatchPlan(const WorkList& items, const DispatchOptions& opt);

} // namespace pm
