// SPDX-License-Identifier: MIT
//
// The internal-padding budget of issue #731. Four types carried padding that a widest-member-first
// order removes; the rest of the issue's padding rows are tail padding that no reorder can remove,
// and they are recorded as accepted in docs/design/performance.md rather than asserted here.
//
// These are `static_assert`s, not runtime checks: a size regression is a build failure, the same
// rule `node.hpp` applies to `kNodeSizeBudget`. The numbers are also pinned as `enforce` rows in
// tests/golden/perf/memory_budget.json, which catches a change on a lane that does not compile this
// test.

#include "tessera/runtime/frame_counters.hpp"
#include "tessera/runtime/frame_pacer.hpp"
#include "tessera/ui/overlay/placement.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <cstddef>
#include <iostream>

namespace runtime = tessera::runtime;
namespace ui = tessera::ui;

namespace {

// Each budget is the measured size with no headroom: these are ABI constants, and the point of the
// row is that a member added without re-checking the order fails the build.
constexpr std::size_t kPaceDecisionBytes = 16;
constexpr std::size_t kAdaptiveQualitySampleBytes = 24;
constexpr std::size_t kOverlayPlacementBytes = 40;
constexpr std::size_t kPaintDataBytes = 528;

static_assert(sizeof(runtime::PaceDecision) == kPaceDecisionBytes,
              "sizeof(PaceDecision) regressed (issue #731: order members widest-first)");
static_assert(sizeof(runtime::AdaptiveQualitySample) == kAdaptiveQualitySampleBytes,
              "sizeof(AdaptiveQualitySample) regressed (issue #731: order members widest-first)");
static_assert(sizeof(ui::OverlayPlacement) == kOverlayPlacementBytes,
              "sizeof(OverlayPlacement) regressed (issue #731: order members widest-first)");
static_assert(sizeof(ui::PaintData) == kPaintDataBytes,
              "sizeof(PaintData) regressed (issue #731: LinearGradient reordered)");

} // namespace

int main() {
    bool ok = true;
    const auto check = [&ok](const char* name, std::size_t actual, std::size_t budget) {
        if (actual != budget) {
            std::cerr << "padding_size: " << name << "=" << actual << " budget=" << budget << "\n";
            ok = false;
        }
    };
    check("PaceDecision", sizeof(runtime::PaceDecision), kPaceDecisionBytes);
    check("AdaptiveQualitySample", sizeof(runtime::AdaptiveQualitySample),
          kAdaptiveQualitySampleBytes);
    check("OverlayPlacement", sizeof(ui::OverlayPlacement), kOverlayPlacementBytes);
    check("PaintData", sizeof(ui::PaintData), kPaintDataBytes);
    std::cout << "padding_size: PaceDecision=" << sizeof(runtime::PaceDecision)
              << " AdaptiveQualitySample=" << sizeof(runtime::AdaptiveQualitySample)
              << " OverlayPlacement=" << sizeof(ui::OverlayPlacement)
              << " PaintData=" << sizeof(ui::PaintData) << "\n";
    return ok ? 0 : 1;
}
