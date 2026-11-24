// SPDX-License-Identifier: MIT
//
// `tessera port-check <vite project>` (ADR-058 section 5 item 1, issue #546).
//
// The command reads `<project>/package.json`, classifies every dependency against the pinned class
// table, and prints the report. `--json` switches the rendering; nothing else changes, so a script
// and a person see the same verdict.

#include "port_check.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view kUsage =
    "usage: tessera port-check <project-dir> [--json]\n"
    "\n"
    "  Reads <project-dir>/package.json and prints each dependency's port class\n"
    "  (ADR-058): A source-compatible, B shim, C build-time, D not ported.\n"
    "  Exits 0 when nothing blocks the port, 1 when a class-D dependency does,\n"
    "  and 2 when the manifest cannot be used.\n"
    "\n"
    "  --json   print the machine-readable report instead of the text one\n";

// The whole file, or an error worth printing. A missing `package.json` is the ordinary mistake this
// tool exists to catch, so it is reported as itself rather than as an empty report.
bool read_file(const std::string& path, std::string& out, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot read " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string project;
    bool json = false;
    // ADR-058 spells it `tessera port-check`, and this binary *is* `tessera` today: there is no
    // subcommand dispatcher yet, so the subcommand name may or may not be the first argument. Both
    // spellings work, which is also what lets the test drive the binary directly.
    int first = 1;
    if (argc > 1 && std::string_view(argv[1]) == "port-check") {
        first = 2;
    }
    for (int index = first; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--json") {
            json = true;
            continue;
        }
        if (argument == "--help" || argument == "-h") {
            std::cout << kUsage;
            return 0;
        }
        if (!argument.empty() && argument.front() == '-') {
            std::cerr << "port-check: unknown option " << argument << "\n" << kUsage;
            return 2;
        }
        if (!project.empty()) {
            std::cerr << "port-check: one project at a time\n" << kUsage;
            return 2;
        }
        project = std::string(argument);
    }
    if (project.empty()) {
        std::cerr << kUsage;
        return 2;
    }

    // A trailing slash is what a shell's tab-completion produces, and the report prints the path it
    // was given, so it is normalized here rather than left to make two runs differ.
    while (project.size() > 1 && project.back() == '/') {
        project.pop_back();
    }

    const std::string manifest_path = project + "/package.json";
    std::string text;
    std::string error;
    if (!read_file(manifest_path, text, error)) {
        tessera::port_check::PortReport missing;
        missing.project_path = project;
        missing.error = error;
        std::cout << (json ? tessera::port_check::render_json(missing)
                           : tessera::port_check::render_text(missing));
        return tessera::port_check::exit_code(missing);
    }

    const tessera::port_check::PortReport report =
        tessera::port_check::check_manifest(project, text);
    std::cout << (json ? tessera::port_check::render_json(report)
                       : tessera::port_check::render_text(report));
    return tessera::port_check::exit_code(report);
}
