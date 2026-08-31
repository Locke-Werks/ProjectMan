#include "discovery.h"
#include "git.h"
#include "strutil.h"

#include <algorithm>
#include <system_error>

namespace pm {
namespace {

// The non-throwing form on both construction and increment. The plain range-for
// over a directory_iterator throws from operator++ when it meets an entry it
// cannot stat, and ~/projects contains OneDrive-backed and permission-odd
// directories where that is a real outcome, not a hypothetical one.
std::vector<fs::directory_entry> subdirs(const fs::path& dir)
{
    std::vector<fs::directory_entry> out;

    std::error_code       ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return out;

    const fs::directory_iterator end;
    while (it != end) {
        std::error_code isDirEc;
        if (it->is_directory(isDirEc) && !isDirEc)
            out.push_back(*it);

        it.increment(ec);
        if (ec)
            break;
    }

    std::sort(out.begin(), out.end(),
              [](const fs::directory_entry& a, const fs::directory_entry& b) {
                  return a.path().filename() < b.path().filename();
              });
    return out;
}

bool excluded(std::string_view name, const std::vector<std::string>& list)
{
    return std::any_of(list.begin(), list.end(),
                       [&](const std::string& e) { return iequals(e, name); });
}

Project makeProject(const fs::directory_entry& entry, ProjectKind kind,
                    const fs::path& container)
{
    Project p;
    p.path      = entry.path();
    p.name      = narrow(entry.path().filename().wstring());
    p.kind      = kind;
    p.container = container;

    std::error_code ec;
    p.hasClaudeMd = fs::exists(entry.path() / "CLAUDE.md", ec);

    if (kind == ProjectKind::BareRepo)
        p.git.state = GitState::Bare;
    else if (kind == ProjectKind::Repo)
        p.git.state = GitState::Unknown;   // resolved by the scan
    else
        p.git.state = GitState::NotARepo;

    return p;
}

} // namespace

bool isPruned(std::string_view name)
{
    return std::any_of(std::begin(kPruned), std::end(kPruned),
                       [&](std::string_view p) { return iequals(p, name); });
}

ProjectList discover(const DiscoveryOptions& opts, std::string* error)
{
    ProjectList out;

    std::error_code ec;
    if (!fs::is_directory(opts.root, ec)) {
        if (error)
            *error = "root is not a directory: " + opts.root.string();
        return out;
    }

    for (const fs::directory_entry& entry : subdirs(opts.root)) {
        const std::string name = narrow(entry.path().filename().wstring());

        if (isPruned(name) || excluded(name, opts.exclude))
            continue;

        const ProjectKind kind = git::classify(entry.path());

        if (kind == ProjectKind::Repo || kind == ProjectKind::BareRepo) {
            out.push_back(makeProject(entry, kind, {}));
            // Deliberately no descent. A repo's own subdirectories are its
            // contents, not separate projects, and descending is exactly how
            // the four HHS checkouts get counted more than once.
            continue;
        }

        // Not a repo. Look one level down for repos before deciding whether
        // this is a container or a plain folder.
        std::vector<Project> children;
        if (opts.descendContainers) {
            for (const fs::directory_entry& sub : subdirs(entry.path())) {
                const std::string subName = narrow(sub.path().filename().wstring());
                if (isPruned(subName) || excluded(subName, opts.exclude))
                    continue;

                const ProjectKind subKind = git::classify(sub.path());
                if (subKind == ProjectKind::Repo || subKind == ProjectKind::BareRepo)
                    children.push_back(makeProject(sub, subKind, entry.path()));
            }
        }

        const bool isContainer = !children.empty();

        if (isContainer || opts.includePlain) {
            out.push_back(makeProject(
                entry, isContainer ? ProjectKind::Container : ProjectKind::Plain, {}));
        }

        // A container is a launchable project in its own right, and its
        // children follow it.
        out.insert(out.end(), std::make_move_iterator(children.begin()),
                   std::make_move_iterator(children.end()));
    }

    return out;
}

} // namespace pm
