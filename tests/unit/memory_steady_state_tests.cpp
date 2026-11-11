// SPDX-License-Identifier: MIT
//
// Resident-memory sampler and steady-state baseline (issue #13). The bounds come from
// tests/golden/memory/steady_state.txt (argv[1]): resident bytes per plain node while a tree is
// built, and resident growth across idle cycles after warm-up (a leak guard). Real RSS is machine
// dependent, so the bounds are generous; they catch leaks and order-of-magnitude regressions.

#include "tessera/diagnostics/memory.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

namespace diag = tessera::diagnostics;
namespace ui = tessera::ui;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "memory_steady_state: " << message << "\n";
    }
    return condition;
}

struct Baseline {
    std::uint64_t nodes = 0;
    std::uint64_t idle_cycles = 0;
    std::uint64_t max_bytes_per_node = 0;
    std::uint64_t max_idle_growth_bytes = 0;
};

bool load(const char* path, Baseline& out) {
    std::ifstream file(path);
    std::string line;
    int found = 0;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0, eq);
        const std::uint64_t value = std::strtoull(line.c_str() + eq + 1, nullptr, 10);
        if (key == "nodes") {
            out.nodes = value;
        } else if (key == "idle_cycles") {
            out.idle_cycles = value;
        } else if (key == "max_bytes_per_node") {
            out.max_bytes_per_node = value;
        } else if (key == "max_idle_growth_bytes") {
            out.max_idle_growth_bytes = value;
        } else {
            continue;
        }
        ++found;
    }
    return found == 4 && out.nodes != 0;
}

int sampler() {
    const diag::ProcessMemory memory = diag::sample_process_memory();
    if (!expect(memory.valid, "the platform reports resident memory") ||
        !expect(memory.resident_bytes > 64 * 1024, "a running process holds more than 64 KiB") ||
        !expect(memory.peak_resident_bytes >= memory.resident_bytes, "peak is at least current")) {
        return 1;
    }

    diag::MetricsRegistry registry;
    diag::MemoryGauges gauges(registry);
    const diag::ProcessMemory published = gauges.sample();
    if (!expect(published.valid && registry.value(gauges.resident_id()) == published.resident_bytes,
                "the gauge carries the sampled resident bytes") ||
        !expect(registry.value(gauges.peak_id()) == published.peak_resident_bytes,
                "the gauge carries the sampled peak") ||
        !expect(registry.name(gauges.resident_id()) == "memory.rss_bytes", "gauge name")) {
        return 2;
    }
    return 0;
}

int steady_state(const Baseline& baseline) {
    auto root = std::make_unique<ui::Node>(1);

    const diag::ProcessMemory before = diag::sample_process_memory();
    for (std::uint64_t i = 0; i < baseline.nodes; ++i) {
        root->add_child(std::make_unique<ui::Node>(static_cast<ui::NodeId>(i + 2)));
    }
    const diag::ProcessMemory built = diag::sample_process_memory();
    const std::uint64_t grown = built.resident_bytes > before.resident_bytes
                                    ? built.resident_bytes - before.resident_bytes
                                    : 0;
    std::cout << "memory_steady_state: " << baseline.nodes << " nodes cost " << grown
              << " resident bytes (" << grown / baseline.nodes << " per node)\n";
    if (!expect(grown / baseline.nodes <= baseline.max_bytes_per_node,
                "resident bytes per plain node stay within the baseline")) {
        return 3;
    }

    // Idle cycles: mark and clear the dirty flags the way an idle frame does. Nothing may stay
    // allocated across them.
    const auto cycle = [&] {
        root->mark_dirty(ui::Dirty::Paint);
        root->clear_dirty(ui::Dirty::All);
    };
    cycle();
    const diag::ProcessMemory warm = diag::sample_process_memory();
    for (std::uint64_t i = 0; i < baseline.idle_cycles; ++i) {
        cycle();
    }
    const diag::ProcessMemory steady = diag::sample_process_memory();
    const std::uint64_t idle_growth = steady.resident_bytes > warm.resident_bytes
                                          ? steady.resident_bytes - warm.resident_bytes
                                          : 0;
    std::cout << "memory_steady_state: " << baseline.idle_cycles << " idle cycles grew "
              << idle_growth << " resident bytes\n";
    if (!expect(idle_growth <= baseline.max_idle_growth_bytes,
                "idle cycles do not grow resident memory past the baseline")) {
        return 4;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: memory_steady_state_tests <steady_state.txt>\n";
        return 64;
    }
    Baseline baseline;
    if (!load(argv[1], baseline)) {
        std::cerr << "memory_steady_state: cannot read the baseline " << argv[1] << "\n";
        return 65;
    }
    if (const int rc = sampler(); rc != 0) {
        return rc;
    }
    if (const int rc = steady_state(baseline); rc != 0) {
        return rc;
    }
    std::cout << "memory_steady_state: ok\n";
    return 0;
}
