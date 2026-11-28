// SPDX-License-Identifier: MIT
//
// Build-time tool of the `codegen_pending_tsx` test (#656, docs/specs/pending-ui-v1.md): compiles
// the TSX of `codegen_pending_tsx_fixture.hpp` through the real front end (analysis against a
// services manifest, lowering, the ADR-040 generator) and writes `Feed.g.hpp` into the directory
// given as argv[1]. Exit 1 with the first diagnostic on any failure.

#include "codegen_pending_tsx_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: codegen_pending_tsx_emit <out-dir>\n";
        return 2;
    }
    namespace fixture = tessera::test::pending_tsx;
    std::string error;
    const auto module = fixture::compile(fixture::kSource, error);
    if (!module) {
        std::cerr << error << "\n";
        return 1;
    }
    const auto generated = tessera::compiler::codegen::generate(*module, {}, std::nullopt);
    if (!generated) {
        std::cerr << "codegen: " << generated.error().front() << "\n";
        return 1;
    }
    for (const auto& file : *generated) {
        std::ofstream out(std::filesystem::path(argv[1]) / file.path, std::ios::binary);
        out << file.text;
    }
    return 0;
}
