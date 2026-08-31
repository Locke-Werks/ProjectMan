#include "workitems.h"
#include "strutil.h"

#include <algorithm>
#include <set>
#include <sstream>

namespace pm {
namespace {

std::string plural(int n, const char* singular, const char* pluralForm)
{
    return std::to_string(n) + " " + (n == 1 ? singular : pluralForm);
}

void add(WorkList& out, WorkKind kind, const Project& p, int count,
         std::string summary, std::string detail = {})
{
    WorkItem w;
    w.kind    = kind;
    w.project = p.displayName();
    w.path    = p.path;
    w.count   = count;
    w.summary = std::move(summary);
    w.detail  = std::move(detail);
    out.push_back(std::move(w));
}

} // namespace

const char* kindLabel(WorkKind k)
{
    switch (k) {
    case WorkKind::Uncommitted: return "UNCOMMITTED";
    case WorkKind::Unpushed:    return "UNPUSHED";
    case WorkKind::Unpulled:    return "BEHIND";
    case WorkKind::Stash:       return "STASH";
    case WorkKind::NoUpstream:  return "NO UPSTREAM";
    case WorkKind::OpenPr:      return "PR";
    case WorkKind::OpenIssue:   return "ISSUE";
    case WorkKind::Checklist:   return "CHECKLIST";
    case WorkKind::Unfinished:  return "UNFINISHED";
    }
    return "ITEM";
}

int WorkItem::weight() const
{
    switch (kind) {
    case WorkKind::OpenPr:      return 100;
    case WorkKind::Uncommitted: return 90;
    case WorkKind::Unfinished:  return 80;
    case WorkKind::Checklist:   return 70;
    case WorkKind::OpenIssue:   return 60;
    case WorkKind::Unpushed:    return 50;
    case WorkKind::Stash:       return 30;
    case WorkKind::NoUpstream:  return 20;
    case WorkKind::Unpulled:    return 10;
    }
    return 0;
}

WorkList collectWorkItems(const ProjectList& projects)
{
    WorkList out;

    for (const Project& p : projects) {
        const GitStatus& g = p.git;

        if (g.state == GitState::Dirty) {
            const int n = g.dirtyCount();
            std::ostringstream d;
            d << g.staged << " staged, " << g.unstaged << " modified, " << g.untracked
              << " untracked";
            if (g.conflicted)
                d << ", " << g.conflicted << " conflicted";
            add(out, WorkKind::Uncommitted, p, n,
                plural(n, "uncommitted change", "uncommitted changes") + " on "
                    + (g.branch.empty() ? "HEAD" : g.branch),
                d.str());
        }

        if (g.ahead > 0) {
            add(out, WorkKind::Unpushed, p, g.ahead,
                plural(g.ahead, "commit", "commits") + " ahead of " + g.upstream);
        }

        if (g.behind > 0) {
            add(out, WorkKind::Unpulled, p, g.behind,
                plural(g.behind, "commit", "commits") + " behind " + g.upstream);
        }

        if (g.stashes > 0)
            add(out, WorkKind::Stash, p, g.stashes, plural(g.stashes, "stash", "stashes"));

        // A branch with commits and no upstream at all is work that exists
        // nowhere else. Worth surfacing, but it is a push decision, not a task.
        if (g.state != GitState::Unknown && g.state != GitState::NotARepo
            && g.state != GitState::Bare && !g.hasUpstream && !g.branch.empty()
            && g.branch != "(detached)") {
            add(out, WorkKind::NoUpstream, p, 1,
                "branch " + g.branch + " has no upstream");
        }

        if (p.open.openPrs > 0) {
            add(out, WorkKind::OpenPr, p, p.open.openPrs,
                plural(p.open.openPrs, "open pull request", "open pull requests")
                    + (p.ownerRepo.empty() ? "" : " on " + p.ownerRepo));
        }

        if (p.open.openIssues > 0) {
            add(out, WorkKind::OpenIssue, p, p.open.openIssues,
                plural(p.open.openIssues, "open issue", "open issues")
                    + (p.ownerRepo.empty() ? "" : " on " + p.ownerRepo));
        }

        if (p.open.checklistItems > 0) {
            add(out, WorkKind::Checklist, p, p.open.checklistItems,
                plural(p.open.checklistItems, "unchecked item", "unchecked items"));
        }

        if (!p.open.claudeLastPrompt.empty()) {
            add(out, WorkKind::Unfinished, p, 1,
                p.open.claudeTitle.empty() ? "unfinished Claude Code session"
                                           : p.open.claudeTitle,
                "last prompt: " + p.open.claudeLastPrompt);
        }
    }

    std::stable_sort(out.begin(), out.end(), [](const WorkItem& a, const WorkItem& b) {
        if (a.weight() != b.weight())
            return a.weight() > b.weight();
        return a.count > b.count;
    });

    return out;
}

void preselect(WorkList& items, int maxRepos)
{
    std::set<std::string> repos;

    for (WorkItem& w : items) {
        w.selected = false;

        // Behind, stash and no-upstream are states to notice, not tasks to hand
        // to an agent. Preselecting them would send it off to pull and push.
        const bool actionable = w.kind == WorkKind::Uncommitted
                             || w.kind == WorkKind::OpenPr
                             || w.kind == WorkKind::Checklist
                             || w.kind == WorkKind::Unfinished
                             || w.kind == WorkKind::OpenIssue;
        if (!actionable)
            continue;

        const std::string key = w.path.string();
        if (!repos.count(key) && static_cast<int>(repos.size()) >= maxRepos)
            continue;

        repos.insert(key);
        w.selected = true;
    }
}

DispatchPlan buildDispatchPlan(const WorkList& items, const DispatchOptions& opt)
{
    DispatchPlan plan;

    std::set<std::string> seen;
    for (const WorkItem& w : items) {
        if (!w.selected)
            continue;
        plan.items.push_back(w);
        if (seen.insert(w.path.string()).second)
            plan.repos.push_back(w.path);
    }

    if (plan.items.empty())
        return plan;

    // Group the briefing by repo, because the agent works repo by repo and a
    // flat list of thirty items across nine trees reads as noise.
    std::ostringstream os;
    os << "You are coordinating outstanding work across "
       << plan.repos.size() << (plan.repos.size() == 1 ? " repository" : " repositories")
       << " under this projects tree. Each is available to you via --add-dir.\n\n";

    os << "Outstanding items, as ProjectMan found them:\n\n";

    for (const fs::path& repo : plan.repos) {
        os << "## " << repo.filename().string() << "\n";
        os << "Path: " << repo.string() << "\n";
        for (const WorkItem& w : plan.items) {
            if (w.path != repo)
                continue;
            os << "  - [" << kindLabel(w.kind) << "] " << w.summary << "\n";
            if (!w.detail.empty())
                os << "      " << w.detail << "\n";
        }
        os << "\n";
    }

    os << "How to work:\n\n";
    os << "1. Take the repositories one at a time. Read the actual state before "
          "changing anything: this briefing is a snapshot and may be stale.\n";
    os << "2. Where the right next step is clear, do it.\n";
    os << "3. Where it is not clear, or where a change would be destructive, "
          "irreversible, or a judgement call that is mine to make, stop and ask "
          "me. Asking is expected, not a failure.\n";

    if (opt.allowCommit) {
        os << "4. Commit finished work in the repository it belongs to, with a "
              "message in the imperative mood and no AI attribution trailer of "
              "any kind.\n";
    } else {
        os << "4. Leave changes in the working tree. Do not commit.\n";
    }

    if (!opt.allowPush) {
        os << "5. Do not push, do not open or merge pull requests, and do not "
              "write to GitHub. Everything stays on this machine for review.\n";
    } else {
        os << "5. You may push to a branch. Do not merge to a default branch "
              "without asking.\n";
    }

    os << "6. Do not start work that was not listed above. If you find something "
          "else worth doing, tell me rather than doing it.\n\n";
    os << "Begin by summarising what you intend to do in each repository, then "
          "work through them.\n";

    plan.briefing = os.str();
    return plan;
}

} // namespace pm
