// SPDX-License-Identifier: MIT
//
// The resolved background-image value's size budget (issue #731). `GradientValue` used to carry all
// three gradient shapes side by side, so a value paid for 24 stops where a gradient can use 8:
// `BackgroundLayerList` was 4552 bytes, and `resolve_layers` returns one by value per painted node.
// The three shapes are now a union.
//
// These are `static_assert`s, not runtime checks: a size regression is a build failure, the same
// rule `node.hpp` applies to `kNodeSizeBudget`. The number is also pinned as an `enforce` row in
// tests/golden/perf/memory_budget.json, which catches a change on a lane that does not compile this
// test.

#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <cstddef>
#include <iostream>

namespace style = tessera::ui::style;

namespace {

// The measured size with no headroom: this is an ABI constant, and the point of the row is that a
// shape added back outside the union fails the build.
constexpr std::size_t kBackgroundLayerListBytes = 1672;
constexpr std::size_t kGradientValueBytes = 200;

static_assert(sizeof(style::BackgroundLayerList) == kBackgroundLayerListBytes,
              "sizeof(BackgroundLayerList) regressed (issue #731: the three shapes share a union)");
static_assert(sizeof(style::GradientValue) == kGradientValueBytes,
              "sizeof(GradientValue) regressed (issue #731: the three shapes share a union)");

// The union only pays while it is smaller than the sum of the shapes it replaces. If this ever
// stops holding, the union has stopped earning its keep and the design should be revisited.
static_assert(sizeof(style::GradientValue) < sizeof(style::LinearGradientValue) +
                                                 sizeof(style::RadialGradientValue) +
                                                 sizeof(style::ConicGradientValue),
              "the shape union no longer saves anything over carrying all three");

} // namespace

int main() {
    bool ok = true;
    const auto check = [&ok](const char* name, std::size_t actual, std::size_t budget) {
        if (actual != budget) {
            std::cerr << "background_layer_size: " << name << "=" << actual << " budget=" << budget
                      << "\n";
            ok = false;
        }
    };
    check("BackgroundLayerList", sizeof(style::BackgroundLayerList), kBackgroundLayerListBytes);
    check("GradientValue", sizeof(style::GradientValue), kGradientValueBytes);
    std::cout << "background_layer_size: BackgroundLayerList=" << sizeof(style::BackgroundLayerList)
              << " GradientValue=" << sizeof(style::GradientValue) << "\n";
    return ok ? 0 : 1;
}
