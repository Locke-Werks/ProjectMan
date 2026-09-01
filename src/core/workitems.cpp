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
    w.project   = p.displayName();
    w.ownerRepo = p.ownerRepo;
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
    std::set<std::string> remoteWork;

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

        // A pull request or issue belongs to the remote, not to a working tree,
        // and several repositories here are checked out two or three times.
        // Selecting both BitsyGo and security-reviews/BitsyGo would send the
        // agent at the same two pull requests twice, in two directories.
        // Uncommitted work is the opposite: it exists only in one tree, so it
        // is never deduplicated this way.
        const bool remote = w.kind == WorkKind::OpenPr || w.kind == WorkKind::OpenIssue;
        if (remote && !w.ownerRepo.empty()) {
            const std::string key = toLower(w.ownerRepo) + "/"
                                  + std::to_string(static_cast<int>(w.kind));
            if (!remoteWork.insert(key).second)
                continue;
        }

        const std::string key = w.path.string();
        if (!repos.count(key) && static_cast<int>(repos.size()) >= maxRepos)
            continue;

        repos.insert(key);
        w.selected = true;
    }
}

std::string dispatchSummary(Autonomy a)
{
    switch (a) {
    case Autonomy::Suggest: return "Will report only, and change nothing.";
    case Autonomy::Write:   return "Will edit the working tree, never commit.";
    case Autonomy::Commit:  return "Will commit, never push.";
    case Autonomy::Push:    return "Will commit and push a branch.";
    case Autonomy::Full:    return "Will commit, push, and open pull requests.";
    }
    return {};
}

DispatchPlan buildDispatchPlan(const WorkList& items, const DispatchOptions& opt)
{
    DispatchPlan plan;

    std::set<std::string> seen;
    for (const WorkItem& w : items) {
        if (!w.selected)
            continue;
        // A briefing long enough to bury its own instructions is worse than one
        // that says it was truncated.
        if (opt.maxItems > 0 && static_cast<int>(plan.items.size()) >= opt.maxItems)
            break;
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

    // Above the items, because an instruction that only turns up after a list of
    // thirty findings is an instruction the agent reads as an afterthought.
    if (!opt.instructions.empty()) {
        os << "What I want done on this run. This governs everything below:\n\n"
           << opt.instructions << "\n\n";

        os << "The list that follows is context, not the job. Where it and the "
              "instruction above point in different directions, the instruction "
              "wins, and an item that has nothing to do with it stays untouched.\n\n";
    }

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

    // The autonomy ladder, spelled out. Each rung states its own ceiling
    // explicitly rather than leaving it to be inferred from silence, because
    // "it was not forbidden" is exactly how an agent talks itself past one.
    //
    // Collected first and numbered on the way out, so the lower rungs, which
    // have one rule fewer, do not emit a list that skips a number.
    std::vector<std::string> rules;
    rules.emplace_back(
        "Take the repositories one at a time. Read the actual state before "
        "changing anything: this briefing is a snapshot and may be stale.");

    switch (opt.autonomy) {
    case Autonomy::Suggest:
        rules.emplace_back("Change nothing. Read, and report what you would do "
                           "and why.");
        rules.emplace_back("Do not edit files, do not run commands that write, "
                           "do not commit, and do not touch GitHub.");
        break;

    case Autonomy::Write:
        rules.emplace_back("Where the right next step is clear, make the change.");
        rules.emplace_back("Leave everything in the working tree. Do not commit, "
                           "do not push, and do not touch GitHub. I want to read "
                           "it as a diff.");
        break;

    case Autonomy::Commit:
        rules.emplace_back("Where the right next step is clear, do it.");
        rules.emplace_back("Commit finished work in the repository it belongs "
                           "to, with a message in the imperative mood and no AI "
                           "attribution trailer of any kind.");
        rules.emplace_back("Do not push, do not open or merge pull requests, and "
                           "do not write to GitHub. Everything stays on this "
                           "machine for review.");
        break;

    case Autonomy::Push:
        rules.emplace_back("Where the right next step is clear, do it.");
        rules.emplace_back("Commit with a message in the imperative mood and no "
                           "AI attribution trailer of any kind.");
        rules.emplace_back("Push to a branch of your own. Never push to a default "
                           "branch, never force-push, and do not open pull "
                           "requests.");
        break;

    case Autonomy::Full:
        rules.emplace_back("Where the right next step is clear, do it.");
        rules.emplace_back("Commit with a message in the imperative mood and no "
                           "AI attribution trailer of any kind.");
        rules.emplace_back("Push to a branch and open a pull request against the "
                           "default branch. Never merge one, never force-push, "
                           "and never push to a default branch directly.");
        break;
    }

    rules.emplace_back("Where a step is unclear, or a change would be "
                       "destructive, irreversible, or a judgement call that is "
                       "mine to make, stop and ask me. Asking is expected, not a "
                       "failure.");
    // Unqualified, this rule forbids exactly what an instruction like "rename
    // every default branch" asks for, and the agent would be right to obey the
    // rule and refuse. It has to name the instruction as a source of work.
    if (opt.instructions.empty()) {
        rules.emplace_back("Do not start work that was not listed above. If you "
                           "find something else worth doing, tell me rather than "
                           "doing it.");
    } else {
        rules.emplace_back("My instruction at the top is a source of work in its "
                           "own right: do what it asks even where no item below "
                           "mentions it. Beyond that and the items listed, start "
                           "nothing. If you find something else worth doing, tell "
                           "me rather than doing it.");
    }

    os << "How to work:\n\n";
    for (size_t i = 0; i < rules.size(); ++i)
        os << (i + 1) << ". " << rules[i] << "\n";

    os << "\nBegin by summarising what you intend to do in each repository, then "
          "work through them.\n";

    plan.briefing = os.str();
    return plan;
}

} // namespace pm
