#pragma once

#include "config.h"
#include "json.h"
#include "model.h"

#include <string>

namespace pm::mcp {

// Runs the same sweep both ProjectMan front ends run, and returns the result as
// one JSON object.
//
// Read-only in every sense that matters: nothing is launched, nothing is
// written, and the config is loaded but never created. That last part is why
// this product declares ProjectMan as a prerequisite rather than falling back
// to defaults: inventing a projects root and reporting on it would be worse
// than saying the setup is missing.
struct ScanOutcome {
    bool        ok = false;
    std::string error;      // set when ok is false, already phrased for a person
    json::Value payload;    // the tool result when ok
};

ScanOutcome collectProjects();

// Exposed for the sake of being testable without a filesystem full of repos.
json::Value projectToJson(const Project& p);

} // namespace pm::mcp
