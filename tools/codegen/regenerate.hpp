// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

// The codegen CLI driver, shared by tools/codegen/main.cpp and the CLI tests. Kept separate from
// the tessera_codegen library so the library keeps its IR-only dependency (AGENTS.md non-negotiable
// 4); the driver is the one place that may read JSX and call the front end.
namespace tessera::compiler::codegen::cli {

struct Options {
    std::vector<std::string> inputs; // .tsx files, as given on the command line
    std::string output_dir;
    std::string module_name; // --namespace; empty derives it from the first input
    bool check_only = false; // --check (mutually exclusive with the default --write)
    bool verbose = false;
    // --salt: the tessera.json `namespace`. When set, every component and node gets its numeric
    // identity (docs/specs/automation-ids-v1.md section 2) and the IR must validate completely;
    // when empty the identity stays unassigned, as before.
    std::string salt;
    // --root: the project root. With a salt, source paths in the ids are relative to it (default:
    // the directory of the first input), so ids do not depend on where the project is checked out.
    std::string root;
};

struct Message {
    bool is_error = false;
    std::string text;
};

struct Outcome {
    // 0: ok; 1: diagnostics or drift; 2: usage/setup.
    int exit_code = 0;
    std::vector<Message> messages;
};

// Regenerates the C++ of one project. Deterministic: first-error-only diagnostics, no partial
// output, and files are rewritten only when their merged content changed.
[[nodiscard]] Outcome run(const Options& options);

} // namespace tessera::compiler::codegen::cli
