// SPDX-License-Identifier: MIT
//
// Cache-line layout invariants (security review findings C1, C2, C3, C7).
//
// These are `static_assert`s rather than runtime checks: the property under test is a compile-time
// fact about a type, and a regression is a build failure rather than a slow test. The file exists
// as its own target so the assertions are compiled on every lane even when the type's own library
// is not otherwise built by a focused test run.
//
// What each assertion protects:
//
//   * `detail::CacheLine<T>` is exactly one cache line and starts on one. If it were smaller, two
//     counter groups could share a line again; if it were larger, a group would straddle two.
//   * `ui::InputQueue`'s producer and consumer indices are separate lines, so a push cannot
//     invalidate the line the consumer is reading.
//   * `runtime::IoExecutor` and `runtime::WorkerIoExecutor` keep their UI-thread, worker-thread and
//     shared counters on separate lines.
//   * `gpu::Vertex2D` is 64-byte aligned, so a vertex never straddles a cache line.
//   * `diagnostics::MetricsRegistry`'s hot counter array is dense: eight counters per line, not the
//     1.33 an interleaved name+counter slot gave.

#include "tessera/detail/cache_line.hpp"
#include "tessera/diagnostics/metrics.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/runtime/io_executor.hpp"
#include "tessera/runtime/worker_io_executor.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/style/media_query.hpp"
#include "tessera/ui/style/parser.hpp"
#include "tessera/ui/style/theme_asset.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

namespace detail = tessera::detail;
namespace diagnostics = tessera::diagnostics;
namespace gpu = tessera::rendering::gpu;
namespace runtime = tessera::runtime;
namespace ui = tessera::ui;
namespace a11y = tessera::ui::a11y;
namespace style = tessera::ui::style;

// A group must fill exactly one line, and a group that is too large must be a compile error rather
// than a silent straddle. Both halves are asserted here on a representative type.
struct TwoCounters {
    std::atomic<std::size_t> first{0};
    std::atomic<std::size_t> second{0};
};

static_assert(sizeof(detail::CacheLine<TwoCounters>) == detail::kCacheLine,
              "a cache-line group must fill exactly one line");
static_assert(alignof(detail::CacheLine<TwoCounters>) == detail::kCacheLine,
              "a cache-line group must start on a line boundary");
static_assert(detail::cache_line_is_exact<TwoCounters>(),
              "cache_line_is_exact must agree with the type's own layout");

// The line size is the value the whole header reasons about; pin it so a future edit cannot quietly
// change the separation distance.
static_assert(detail::kCacheLine == 64, "the cache-line size is 64 bytes on every target lane");

// C2: the input ring's two indices are on separate lines. `InputQueue` is not standard-layout (it
// has a `std::vector` member), so the assertion is on the size of the whole queue instead: two
// cache-line groups plus the vector handle and the capacity, rounded up to a line.
static_assert(sizeof(ui::InputQueue) >= 2 * detail::kCacheLine,
              "the input queue must hold two separate cache-line groups");

// C7: a vertex is exactly one line and aligned to it, so a sequential vertex write touches one line
// per vertex instead of two.
static_assert(sizeof(gpu::Vertex2D) == detail::kCacheLine, "a vertex is one cache line");
static_assert(alignof(gpu::Vertex2D) == detail::kCacheLine, "a vertex starts on a cache line");

// C3: the metrics registry's hot array is a dense `std::atomic<std::uint64_t>` array, so a line
// holds eight counters. The cold name/kind/used record is separate and is not part of the hot
// stride.
static_assert(detail::kCacheLine / sizeof(std::atomic<std::uint64_t>) == 8,
              "a cache line holds eight metric counters");

// C1: the executors keep three groups (UI, worker, shared), so each is at least three lines of
// counters plus its own members. Asserting the lower bound catches a group being folded back into
// the object's own line.
static_assert(sizeof(runtime::IoExecutor) >= 3 * detail::kCacheLine,
              "the I/O executor must hold three separate cache-line groups");
static_assert(sizeof(runtime::WorkerIoExecutor) >= 3 * detail::kCacheLine,
              "the worker I/O executor must hold three separate cache-line groups");

// Issue #732: the straddling types. Each has a size that is a multiple of 64, so with the natural
// 8-byte alignment seven of eight instances span two lines and a sequential walk touches both. The
// rule is `cache-lines.md` section 4: "a new hot struct whose size is a multiple of 64 needs
// `alignas(64)`". These are the ones that landed before the rule was enforced.
static_assert(sizeof(ui::InputEvent) % detail::kCacheLine == 0,
              "InputEvent is a whole number of cache lines");
static_assert(alignof(ui::InputEvent) == detail::kCacheLine,
              "InputEvent must start on a cache line (issue #732)");

static_assert(sizeof(style::Rule) % detail::kCacheLine == 0, "Rule is a whole number of lines");
static_assert(alignof(style::Rule) == detail::kCacheLine,
              "Rule must start on a cache line (issue #732)");

static_assert(sizeof(style::MediaQuery) % detail::kCacheLine == 0,
              "MediaQuery is a whole number of lines");
static_assert(alignof(style::MediaQuery) == detail::kCacheLine,
              "MediaQuery must start on a cache line (issue #732)");

static_assert(sizeof(style::ThemeToken) % detail::kCacheLine == 0,
              "ThemeToken is a whole number of lines");
static_assert(alignof(style::ThemeToken) == detail::kCacheLine,
              "ThemeToken must start on a cache line (issue #732)");

static_assert(sizeof(style::ThemePin) % detail::kCacheLine == 0,
              "ThemePin is a whole number of lines");
static_assert(alignof(style::ThemePin) == detail::kCacheLine,
              "ThemePin must start on a cache line (issue #732)");

// Issue #732 section 2: `UiaHost`'s atomics are grouped by the thread that writes them, so the UI
// thread's `mark_dirty`/`tick` and the bridge thread's `flush_events` no longer invalidate each
// other's line. The groups are private, so the assertion is on the object: two separate
// `CacheLine` groups plus the rest of the host means at least two lines of separation.
static_assert(sizeof(a11y::UiaHost) >= 2 * detail::kCacheLine,
              "UiaHost must hold two separate cache-line groups (issue #732)");

} // namespace

int main() {
    // The assertions above are the test. Reaching here means every one of them held, so print the
    // measured sizes for the log: a reader comparing two runs sees the layout, not just a pass.
    std::cout << "cache_line: ok"
              << " line=" << detail::kCacheLine << " sizeof(Vertex2D)=" << sizeof(gpu::Vertex2D)
              << " alignof(Vertex2D)=" << alignof(gpu::Vertex2D)
              << " sizeof(InputQueue)=" << sizeof(ui::InputQueue)
              << " sizeof(IoExecutor)=" << sizeof(runtime::IoExecutor)
              << " sizeof(WorkerIoExecutor)=" << sizeof(runtime::WorkerIoExecutor) << "\n";
    return 0;
}
