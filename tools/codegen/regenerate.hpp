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
    // --salt: overrides the id salt. By default the salt is `app.namespace` of the tessera.json at
    // --root or found above the first input (docs/specs/project-config-v1.md); with neither, the
    // identity stays unassigned. With a salt every component and node gets its numeric identity
    // (docs/specs/automation-ids-v1.md section 2) and the IR must validate completely.
    std::string salt;
    // --root: the project root that source paths in the ids are relative to (default: the
    // directory of the tessera.json that was found, else of the first input), so ids do not depend
    // on where the project is checked out.
    std::string root;
    // --emit-ir FILE: write the canonical IR JSON of the lowered module and generate no C++
    // (docs/specs/ir-v1.md). --out is not required.
    std::string emit_ir_path;
    // --emit-library-cpp FILE: write a C++ translation unit that rebuilds the lowered components as
    // IR data. This is the build-time path for tessera/components/*.tsx (issue #70); --out is not
    // required. The TSX sources are the source of truth, so an unlowerable source fails the run.
    std::string emit_library_cpp_path;
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
