// SPDX-License-Identifier: MIT
//
// The one viewport-unit table (issue #691). See the header for why it exists and what the
// small/large/dynamic variants mean.

#include "tessera/ui/style/viewport_units.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace tessera::ui::style {
namespace {

struct UnitEntry {
    std::string_view suffix;
    ViewportSize size;
    ViewportAxis axis;
};

// The unit suffixes. `vmin`/`vmax` are the min/max of the two axes; the `s`/`l`/`d` prefixes are
// the small/large/dynamic viewport. The `vi`/`vb` inline/block-axis forms are deliberately absent:
// they need the writing mode, which a length resolver does not have, so a caller that has it maps
// them before asking here.
constexpr std::array<UnitEntry, 16> kUnits = {{
    // Plain `v*`.
    {"vw", ViewportSize::Default, ViewportAxis::Width},
    {"vh", ViewportSize::Default, ViewportAxis::Height},
    {"vmin", ViewportSize::Default, ViewportAxis::Min},
    {"vmax", ViewportSize::Default, ViewportAxis::Max},
    // Small viewport: the platform's chrome is shown.
    {"svw", ViewportSize::Small, ViewportAxis::Width},
    {"svh", ViewportSize::Small, ViewportAxis::Height},
    {"svmin", ViewportSize::Small, ViewportAxis::Min},
    {"svmax", ViewportSize::Small, ViewportAxis::Max},
    // Large viewport: the chrome is hidden.
    {"lvw", ViewportSize::Large, ViewportAxis::Width},
    {"lvh", ViewportSize::Large, ViewportAxis::Height},
    {"lvmin", ViewportSize::Large, ViewportAxis::Min},
    {"lvmax", ViewportSize::Large, ViewportAxis::Max},
    // Dynamic viewport: tracks the chrome as it moves.
    {"dvw", ViewportSize::Dynamic, ViewportAxis::Width},
    {"dvh", ViewportSize::Dynamic, ViewportAxis::Height},
    {"dvmin", ViewportSize::Dynamic, ViewportAxis::Min},
    {"dvmax", ViewportSize::Dynamic, ViewportAxis::Max},
}};

// The extents for one size. A viewport without dynamic chrome reports the same extent for all
// three, which is the correct answer for a desktop window rather than a fallback.
struct Extents {
    double width = 0.0;
    double height = 0.0;
};

Extents extents_for(ViewportSize size, const ViewportUnits& viewport) noexcept {
    if (!viewport.has_dynamic_chrome) {
        return Extents{viewport.width, viewport.height};
    }
    switch (size) {
    case ViewportSize::Small:
        return Extents{viewport.small_width > 0.0 ? viewport.small_width : viewport.width,
                       viewport.small_height > 0.0 ? viewport.small_height : viewport.height};
    case ViewportSize::Large:
        return Extents{viewport.large_width > 0.0 ? viewport.large_width : viewport.width,
                       viewport.large_height > 0.0 ? viewport.large_height : viewport.height};
    case ViewportSize::Default:
    case ViewportSize::Dynamic:
        break;
    }
    return Extents{viewport.width, viewport.height};
}

} // namespace

bool parse_viewport_unit(std::string_view unit, ViewportSize& size, ViewportAxis& axis) noexcept {
    for (const UnitEntry& entry : kUnits) {
        if (entry.suffix == unit) {
            size = entry.size;
            axis = entry.axis;
            return true;
        }
    }
    return false;
}

double viewport_extent(ViewportSize size, ViewportAxis axis,
                       const ViewportUnits& viewport) noexcept {
    const Extents extents = extents_for(size, viewport);
    switch (axis) {
    case ViewportAxis::Width:
        return extents.width;
    case ViewportAxis::Height:
        return extents.height;
    case ViewportAxis::Min:
        return std::min(extents.width, extents.height);
    case ViewportAxis::Max:
        return std::max(extents.width, extents.height);
    }
    return 0.0;
}

double resolve_viewport_length(const ViewportLength& length,
                               const ViewportUnits& viewport) noexcept {
    return viewport_extent(length.size, length.axis, viewport) * length.value / 100.0;
}

} // namespace tessera::ui::style
