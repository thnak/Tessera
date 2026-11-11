// SPDX-License-Identifier: MIT

// tessera_codegen: regenerate C++ from JSX/TSX through IR (docs/guides/developer-workflow.md).
//
// Exit codes: 0 ok; 1 diagnostics or drift; 2 usage/setup.
// `--check` validates that the output directory is current and never writes (CI gate);
// the default and `--write` regenerate, merging each file with its existing developer regions.

#include "regenerate.hpp"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kUsage =
    "usage: tessera_codegen --out DIR [--check|--write] [--namespace NAME] [--salt SALT] "
    "[--root DIR] [--verbose] FILE.tsx...\n"
    "       tessera_codegen --emit-ir FILE [options] FILE.tsx...\n"
    "       tessera_codegen --emit-library-cpp FILE [options] FILE.tsx...\n"
    "\n"
    "  --out DIR         output directory for the generated .hpp/.cpp files\n"
    "  --check           fail (exit 1) when DIR is out of date; writes nothing\n"
    "  --write           regenerate and merge with existing developer regions (default)\n"
    "  --emit-ir FILE    write the canonical IR JSON of the lowered module and no C++\n"
    "  --ir-out FILE     alias of --emit-ir\n"
    "  --emit-library-cpp FILE\n"
    "                    write the build-time C++ translation unit that rebuilds the lowered\n"
    "                    library components as IR data (no --out needed)\n"
    "  --namespace NAME  C++ namespace / IR module name (default: first file stem)\n"
    "  --salt SALT       id salt, overriding `app.namespace` of tessera.json (found at --root or\n"
    "                    above the first input); with a salt every node gets a numeric uiId\n"
    "  --root DIR        project root that source paths in the ids are relative to (default: the\n"
    "                    directory of tessera.json, else of the first input)\n"
    "  --verbose         report every file as written or unchanged\n";

int usage_error(std::string_view message) {
    std::cerr << "tessera_codegen: " << message << "\n\n" << kUsage;
    return 2;
}

} // namespace

int main(int argc, char** argv) {
    tessera::compiler::codegen::cli::Options options;
    bool help = false;
    bool have_out = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            help = true;
        } else if (arg == "--check") {
            options.check_only = true;
        } else if (arg == "--write") {
            options.check_only = false;
        } else if (arg == "--verbose") {
            options.verbose = true;
        } else if (arg == "--out" || arg == "--namespace" || arg == "--salt" || arg == "--root" ||
                   arg == "--emit-ir" || arg == "--ir-out" || arg == "--emit-library-cpp") {
            if (i + 1 >= argc) {
                return usage_error("`" + std::string(arg) + "` needs a value");
            }
            const std::string value(argv[++i]);
            if (arg == "--out") {
                options.output_dir = value;
                have_out = true;
            } else if (arg == "--salt") {
                options.salt = value;
            } else if (arg == "--root") {
                options.root = value;
            } else if (arg == "--emit-ir" || arg == "--ir-out") {
                options.emit_ir_path = value;
            } else if (arg == "--emit-library-cpp") {
                options.emit_library_cpp_path = value;
            } else {
                options.module_name = value;
            }
        } else if (!arg.empty() && arg.front() == '-') {
            return usage_error("unknown option `" + std::string(arg) + "`");
        } else {
            options.inputs.emplace_back(arg);
        }
    }

    if (help) {
        std::cout << kUsage;
        return 0;
    }
    const bool emits = !options.emit_ir_path.empty() || !options.emit_library_cpp_path.empty();
    if (!have_out && !emits) {
        return usage_error("--out is required");
    }
    if (options.inputs.empty()) {
        return usage_error("no input .tsx file");
    }

    const tessera::compiler::codegen::cli::Outcome outcome =
        tessera::compiler::codegen::cli::run(options);
    for (const auto& message : outcome.messages) {
        if (message.is_error) {
            std::cerr << message.text << "\n";
        } else {
            std::cout << message.text << "\n";
        }
    }
    return outcome.exit_code;
}
