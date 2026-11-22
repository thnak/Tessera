// SPDX-License-Identifier: MIT

#include "benchmark_fixture.hpp"
#include "benchmark_walk.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace tessera::benchmark {
namespace {

// One hardware counter, enabled around a phase. A failed open (no permission, a VM without a PMU,
// a non-Linux host) leaves it closed and the column reads "n/a".
class Counter {
public:
    Counter() = default;
    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    ~Counter() {
#if defined(__linux__)
        if (fd_ >= 0) {
            close(fd_);
        }
#endif
    }

    // `type`/`config` are the perf_event_attr fields; both are ignored off Linux.
    void open(std::uint32_t type, std::uint64_t config) {
#if defined(__linux__)
        perf_event_attr attr{};
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = 1;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        fd_ = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
#else
        (void)type;
        (void)config;
#endif
    }
    void start() {
#if defined(__linux__)
        if (fd_ >= 0) {
            ioctl(fd_, PERF_EVENT_IOC_RESET, 0);
            ioctl(fd_, PERF_EVENT_IOC_ENABLE, 0);
        }
#endif
    }
    void stop() {
#if defined(__linux__)
        if (fd_ >= 0) {
            ioctl(fd_, PERF_EVENT_IOC_DISABLE, 0);
        }
#endif
    }
    // -1 when the counter is unavailable.
    [[nodiscard]] long long read_value() const {
#if defined(__linux__)
        if (fd_ >= 0) {
            std::uint64_t value = 0;
            if (read(fd_, &value, sizeof(value)) == static_cast<ssize_t>(sizeof(value))) {
                return static_cast<long long>(value);
            }
        }
#endif
        return -1;
    }

private:
    int fd_ = -1;
};

struct Counters {
    Counter l1d_miss;
    Counter cache_miss;
    Counter instructions;

    Counters() {
#if defined(__linux__)
        constexpr std::uint64_t kL1dReadMiss = PERF_COUNT_HW_CACHE_L1D |
                                               (PERF_COUNT_HW_CACHE_OP_READ << 8) |
                                               (PERF_COUNT_HW_CACHE_RESULT_MISS << 16);
        l1d_miss.open(PERF_TYPE_HW_CACHE, kL1dReadMiss);
        cache_miss.open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES);
        instructions.open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS);
#endif
    }
    void start() {
        l1d_miss.start();
        cache_miss.start();
        instructions.start();
    }
    void stop() {
        l1d_miss.stop();
        cache_miss.stop();
        instructions.stop();
    }
};

struct TreeShape {
    std::size_t nodes = 0;
    std::size_t max_depth = 0;
    std::size_t depth_sum = 0;
    std::size_t leaves = 0;
    std::size_t scrollable = 0;
};

void measure_shape(const ui::Node& node, std::size_t depth, TreeShape& shape) {
    ++shape.nodes;
    shape.max_depth = std::max(shape.max_depth, depth);
    shape.depth_sum += depth;
    shape.scrollable += (node.scrollable_x() || node.scrollable_y()) ? 1 : 0;
    if (node.children().empty()) {
        ++shape.leaves;
    }
    for (const auto& child : node.children()) {
        measure_shape(*child, depth + 1, shape);
    }
}

// The lower bound for any traversal: the hot fields a pass reads at every node, and nothing else.
std::uint64_t hot_walk(const ui::Node& node) {
    std::uint64_t sum = node.rendered() ? 1 : 0;
    sum += static_cast<std::uint64_t>(node.dirty());
    sum += static_cast<std::uint64_t>(node.bounds().width > 0.0f);
    for (const auto& child : node.children()) {
        sum += hot_walk(*child);
    }
    return sum;
}

using Clock = std::chrono::steady_clock;

[[nodiscard]] double elapsed_ns(Clock::time_point begin, Clock::time_point end) {
    return std::chrono::duration<double, std::nano>(end - begin).count();
}

struct PhaseResult {
    double best_ns = 0.0;
    double median_ns = 0.0;
    long long l1d = -1;
    long long llc = -1;
    long long instructions = -1;
};

// Runs `body` `rounds` times; the per-round time is the wall time of one call, the counters are the
// totals over every round (divided by the caller). `before` runs untimed ahead of each round.
template<typename Before, typename Body>
PhaseResult run_phase(int rounds, Before&& before, Body&& body) {
    Counters counters;
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(rounds));
    long long l1d = 0;
    long long llc = 0;
    long long ins = 0;
    bool have_counters = true;
    for (int round = 0; round < rounds; ++round) {
        before();
        counters.start();
        const auto begin = Clock::now();
        body();
        const auto end = Clock::now();
        counters.stop();
        samples.push_back(elapsed_ns(begin, end));
        const long long a = counters.l1d_miss.read_value();
        const long long b = counters.cache_miss.read_value();
        const long long c = counters.instructions.read_value();
        if (a < 0 || b < 0 || c < 0) {
            have_counters = false;
        } else {
            l1d += a;
            llc += b;
            ins += c;
        }
    }
    std::sort(samples.begin(), samples.end());
    PhaseResult result;
    result.best_ns = samples.front();
    result.median_ns = samples[samples.size() / 2];
    if (have_counters) {
        result.l1d = l1d / rounds;
        result.llc = llc / rounds;
        result.instructions = ins / rounds;
    }
    return result;
}

void print_row(const char* name, const PhaseResult& result, double units, const char* unit_name) {
    char l1d[24] = "n/a";
    char llc[24] = "n/a";
    char ins[24] = "n/a";
    if (result.l1d >= 0) {
        std::snprintf(l1d, sizeof(l1d), "%.2f", static_cast<double>(result.l1d) / units);
        std::snprintf(llc, sizeof(llc), "%.2f", static_cast<double>(result.llc) / units);
        std::snprintf(ins, sizeof(ins), "%.1f", static_cast<double>(result.instructions) / units);
    }
    std::printf("walk: %-18s best %9.1f us  median %9.1f us  %7.2f ns/%s  L1D-miss/%s %s  "
                "LLC-miss/%s %s  instr/%s %s\n",
                name, result.best_ns / 1000.0, result.median_ns / 1000.0, result.best_ns / units,
                unit_name, unit_name, l1d, unit_name, llc, unit_name, ins);
}

} // namespace

int run_scene_walk(int rounds) {
    rounds = std::max(rounds, 3);
    Fixture fixture;
    // One full scenario lays the scene out and settles the styles, so bounds and transforms are the
    // ones a frame would hit test against.
    (void)fixture.measure();
    ui::Node& root = fixture.root();

    TreeShape shape;
    measure_shape(root, 0, shape);
    std::printf("walk: sizeof(Node) %zu, nodes %zu, leaves %zu, scrollable %zu, max depth %zu, "
                "mean depth %.2f, rounds %d\n",
                sizeof(ui::Node), shape.nodes, shape.leaves, shape.scrollable, shape.max_depth,
                static_cast<double>(shape.depth_sum) / static_cast<double>(shape.nodes), rounds);

    std::uint64_t sink = 0;
    const double nodes = static_cast<double>(shape.nodes);

    // 1. The hot-field walk: what a traversal costs when it reads nothing but what it must.
    print_row("hot walk", run_phase(rounds, [] {}, [&] { sink += hot_walk(root); }), nodes, "node");

    // 2. The dirty pass over a fully dirty paint domain (the worst case a repaint-everything frame
    // hits). The marking is untimed; collect + clear is the pass itself.
    ui::DirtyPass pass(shape.nodes + 16);
    print_row("dirty collect+clear",
              run_phase(
                  rounds, [&] { root.mark_subtree_dirty(ui::Dirty::Paint); },
                  [&] {
                      pass.reset();
                      pass.collect(root, ui::Dirty::Paint);
                      sink += pass.nodes().size();
                      pass.clear(root, ui::Dirty::Paint);
                  }),
              nodes, "node");

    // 3. Hit testing over a fixed grid across the viewport: 16 x 10 = 160 points, row-major, so a
    // run is deterministic and exercises both the hit and the miss paths. The grid is small on
    // purpose: `hit_test_deep` has no spatial pruning outside scroll containers, so one query
    // visits most of the tree and 160 queries already cost about two million node visits.
    constexpr int kColumns = 16;
    constexpr int kRows = 10;
    const double points = static_cast<double>(kColumns) * kRows;
    print_row("hit_test_deep",
              run_phase(
                  rounds, [] {},
                  [&] {
                      for (int row = 0; row < kRows; ++row) {
                          for (int col = 0; col < kColumns; ++col) {
                              const float x =
                                  (static_cast<float>(col) + 0.5f) * (kBenchmarkWidth / kColumns);
                              const float y =
                                  (static_cast<float>(row) + 0.5f) * (kBenchmarkHeight / kRows);
                              const ui::Node* hit = root.hit_test_deep(x, y);
                              sink += hit != nullptr ? hit->id() : 0;
                          }
                      }
                  }),
              points, "point");

    // Keeps the optimizer from deleting the phases; also a cheap determinism fingerprint (the same
    // scene and grid give the same hit ids, so a change here means a behavior change).
    std::printf("walk: checksum %llu\n", static_cast<unsigned long long>(sink));
    return 0;
}

} // namespace tessera::benchmark
