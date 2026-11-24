// SPDX-License-Identifier: MIT
//
// `tessera port-check` (ADR-058 section 5 item 1, issue #546): reads a Vite project's
// `package.json`, classifies each dependency, and prints where the port stands.
//
// **It never reads `node_modules`.** ADR-058 section 4 makes that a property of the whole approach
// - the checker answers from the manifest and the pinned class table, so it runs on a checkout with
// no install, costs nothing, and cannot be influenced by what happens to be on disk. That is also
// why the output is a report rather than a build: a port author wants the list and the blockers
// before they start editing.
//
// The package name is the unit because that is what `package.json` names. Construct-level
// classification (which `dangerouslySetInnerHTML` or `localStorage` a file uses) is the front end's
// job and arrives with rule 1-3 of ADR-058 section 3; this tool's job is to say which dependencies
// are blocking, which is the question a port cannot answer for itself.

#pragma once

#include "class_table.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::port_check {

// One dependency, classified.
struct DependencyReport {
    std::string name;
    // `"dependencies"`, `"devDependencies"` or `"peerDependencies"`: the section it was found in,
    // so the report can explain why a build tool appears in a list about runtime code.
    std::string section;
    bool is_dev = false;
    PortClass port_class = PortClass::NotPorted;
    // What to write instead; empty for classes A and B.
    std::string replacement;
    // The rule the class came from.
    std::string source;
};

// The whole answer for one project.
struct PortReport {
    std::string project_path;
    // Empty when `package.json` parsed. Otherwise why it did not, and the report is not usable.
    std::string error;
    std::vector<DependencyReport> dependencies;
    // Counts per class, indexed by the class letter ('A', 'B', 'C', 'D'), so a caller prints a
    // summary without walking the list.
    std::array<std::size_t, 4> per_class{};
    // The dependencies that block a port: class D only. A build-time (C) entry is work, not a
    // blocker, because the build already does it.
    std::vector<std::string> blockers;
};

// Classifies every dependency of `package_json_text`. `project_path` is copied into the report so
// the caller does not have to keep it alive.
[[nodiscard]] PortReport check_manifest(std::string_view project_path,
                                        std::string_view package_json_text);

// The report as the human-readable text `tessera port-check` prints.
[[nodiscard]] std::string render_text(const PortReport& report);

// The report as one JSON object, for `--json`. Keys are stable; the document is the contract.
[[nodiscard]] std::string render_json(const PortReport& report);

// The exit code the CLI returns: 0 when nothing blocks the port, 1 when a class-D dependency does,
// 2 when the manifest could not be used at all. A caller that pipes the report wants the exit code
// to mean the same thing as `blockers.empty()`.
[[nodiscard]] int exit_code(const PortReport& report);

} // namespace tessera::port_check
