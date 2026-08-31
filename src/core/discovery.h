#pragma once

#include "model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pm {

struct DiscoveryOptions {
    fs::path                 root;
    bool                     descendContainers = true;
    bool                     includePlain      = true;   // show non-repo folders
    std::vector<std::string> exclude;                    // names from [scan] exclude
};

// Directories that a descent never enters.
//
// Not configurable. There are 118 nested .git entries below the top level of
// ~/projects and almost all of them are CMake FetchContent clones under
// build/_deps. Without this the scan reports about sixty projects that are not
// projects.
inline constexpr std::string_view kPruned[] = {
    "build",  "out",   ".venv",  "venv",  "node_modules", "target",
    "_deps",  "dist",  "vendor", ".git",  ".worktrees",   "__pycache__",
    ".next",  ".cache", "third_party", "third-party",
};

bool isPruned(std::string_view name);

// Enumerates DIRECTORIES ONLY, which is why the thirteen loose files at the
// root of ~/projects are structurally out of reach rather than blacklisted by
// name. Two of them are credential files, and a name-based exclusion would go
// stale the moment a third appeared.
//
// Never throws. Descends exactly one level into a non-repo directory to find
// the seventeen repos living inside containers such as security-reviews and
// HHS General Ops, and never descends into a directory already identified as a
// repo, which is what stops the HHS copies of ocio-ato-modernization-site being
// counted three times.
ProjectList discover(const DiscoveryOptions& opts, std::string* error = nullptr);

} // namespace pm
