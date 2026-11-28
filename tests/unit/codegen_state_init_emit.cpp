// SPDX-License-Identifier: MIT
//
// Build-time tool of the `codegen_state_init` test (#1055): compiles the TSX of
// `codegen_state_init_fixture.hpp` through the real front end and the ADR-040 generator and writes
// the generated headers into the directory given as argv[1]. Exit 1 with the first diagnostic on
// any failure.

#include "codegen_state_init_fixture.hpp"
#include "tessera/compiler/codegen/codegen.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: codegen_state_init_emit <out-dir>\n";
        return 2;
    }
    namespace fixture = tessera::test::state_init;
    std::string error;
    const auto module = fixture::compile(error);
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
        const std::filesystem::path path = std::filesystem::path(argv[1]) / file.path;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream out(path, std::ios::binary);
        out << file.text;
    }
    return 0;
}
