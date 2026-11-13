// SPDX-License-Identifier: MIT
//
// Backend-selectable GPU conformance runner (issue #161).
//
//   gpu_conformance [--backend null|vulkan|webgpu] [--summary <path>]
//
// Runs every check against the selected RHI backend, prints one deterministic line per check, and
// optionally writes the deterministic JSON summary. Exit codes: 0 when no check fails (skips are
// fine), 1 when a check fails, 2 for a usage or I/O error. A backend that cannot open a device
// (no Vulkan build, no Vulkan device, WebGPU without a host-provided device) skips cleanly and
// still exits 0 so CI without a GPU is green.

#include "conformance_backend.hpp"
#include "conformance_suite.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace tessera::tests::gpu::conformance;

void print_usage(std::ostream& out) {
    out << "usage: gpu_conformance [--backend null|vulkan|webgpu] [--summary <path>]\n"
           "  --backend   RHI backend to run the checks on (default: null)\n"
           "  --summary   write the deterministic JSON summary to <path>\n";
}

bool write_summary(const std::string& path, const std::string& json) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file << json;
    return file.good();
}

} // namespace

int main(int argc, char** argv) {
    BackendChoice choice = BackendChoice::Null;
    std::string summary_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            return 0;
        }
        if (argument == "--backend") {
            if (index + 1 >= argc) {
                std::cerr << "gpu_conformance: --backend needs a value\n";
                return 2;
            }
            choice = parse_backend(argv[++index]);
            if (choice == BackendChoice::Unknown) {
                std::cerr << "gpu_conformance: unknown backend '" << argv[index] << "'\n";
                return 2;
            }
        } else if (argument == "--summary") {
            if (index + 1 >= argc) {
                std::cerr << "gpu_conformance: --summary needs a path\n";
                return 2;
            }
            summary_path = argv[++index];
        } else {
            std::cerr << "gpu_conformance: unknown argument '" << argument << "'\n";
            print_usage(std::cerr);
            return 2;
        }
    }

    OpenedBackend opened = open_backend(choice);
    const std::string backend(backend_name(choice));
    const Summary summary = opened.available() ? run_suite(*opened.device, opened.profile, backend)
                                               : unavailable_suite(backend);

    for (const CheckOutcome& outcome : summary.checks) {
        std::cout << "gpu_conformance: " << status_name(outcome.status) << ' ' << outcome.name;
        if (!outcome.detail.empty()) {
            std::cout << ": " << outcome.detail;
        }
        std::cout << '\n';
    }

    const SummaryTotals counted = totals(summary);
    std::cout << "gpu_conformance: totals pass=" << counted.pass << " skip=" << counted.skip
              << " fail=" << counted.fail << " total=" << counted.total() << '\n';
    if (!opened.available()) {
        std::cout << "gpu_conformance: skipped (" << opened.unavailable_reason << ")\n";
    }

    if (!summary_path.empty() && !write_summary(summary_path, format_summary(summary))) {
        std::cerr << "gpu_conformance: cannot write summary '" << summary_path << "'\n";
        return 2;
    }
    return has_failure(summary) ? 1 : 0;
}
