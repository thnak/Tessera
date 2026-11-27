// SPDX-License-Identifier: MIT
//
// The small value types `ui::Node` is built from (docs/architecture/god-type-audit-v1.md section
// 3.1): each owns its values and reports "changed" from its setters, with no dirty policy of its
// own.

#include "tessera/ui/scene/node_hints.hpp"
#include "tessera/ui/scene/node_input_policy.hpp"
#include "tessera/ui/scene/node_paint_extent.hpp"
#include "tessera/ui/scene/node_placement.hpp"
#include "tessera/ui/scene/node_scroll_style.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::cerr << "node_parts_tests: FAILED " << what << "\n";
        ++g_failures;
    }
}

using namespace tessera::ui;
namespace rhi = tessera::rendering::rhi;

void hints_are_independent_bits() {
    NodeHints hints;
    check(!hints.has_transform() && !hints.has_item_key(), "hints start clear");
    hints.set_has_item_key(true);
    hints.set_scroll_animation_active(true);
    check(hints.has_item_key() && hints.scroll_animation_active(), "two bits set");
    check(!hints.has_transform() && !hints.has_z_index() && !hints.glass_subtree() &&
              !hints.filter_subtree() && !hints.text_fits_box(),
          "other bits untouched");
    hints.set_filter_subtree(true);
    check(hints.filter_subtree() && !hints.glass_subtree(), "filter and glass subtree are apart");
    hints.set_glass_subtree(true);
    hints.set_filter_subtree(false);
    check(!hints.filter_subtree() && hints.glass_subtree(), "clearing one leaves the other");
    hints.set_glass_subtree(false);
    hints.set_has_item_key(false);
    check(!hints.has_item_key() && hints.scroll_animation_active(), "clear one bit only");
}

void input_policy_reports_changes() {
    NodeInputPolicy policy;
    check(policy.rendered() && policy.pointer_events(), "defaults render and take pointers");
    check(policy.set_visible(false), "visible flip reports a change");
    check(!policy.set_visible(false), "same value reports no change");
    check(!policy.rendered(), "invisible is not rendered");
    policy.set_visible(true);
    policy.set_style_hidden(true);
    check(!policy.rendered(), "style-hidden is not rendered");
}

void scroll_style_reports_changes() {
    NodeScrollStyle style;
    check(!style.scrollable_x() && !style.scrollable_y(), "not scrollable by default");
    check(style.set_scrollable_x(true), "axis flip reports a change");
    check(!style.set_scrollable_x(true), "same axis reports no change");
    check(style.scrollable_x() && !style.scrollable_y(), "axes are independent");
}

void placement_tracks_identity() {
    NodePlacement placement;
    check(placement.transform_is_identity() && placement.is_translation_only(), "identity start");
    check(!placement.set_transform(rhi::Mat3::identity()), "identity again is no change");
    check(placement.set_transform(rhi::Mat3::translation(3.0f, 4.0f)), "translation changes");
    check(!placement.transform_is_identity() && placement.is_translation_only(),
          "translation is translation-only");
    check(placement.set_z_index(2) && !placement.set_z_index(2), "z-index reports changes");
}

void paint_extent_sanitizes_and_caches() {
    NodePaintExtent extent;
    check(!extent.cache_valid(), "cache starts invalid");
    extent.store_box(rhi::Rect{1.0f, 2.0f, 3.0f, 4.0f});
    check(extent.cache_valid() && extent.cached_box().width == 3.0f, "cache stores the box");
    extent.invalidate_cache();
    check(!extent.cache_valid(), "invalidate clears the cache");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    check(extent.set_glass_blur_sigma(5.0f), "sigma change");
    check(extent.set_glass_blur_sigma(-1.0f) && extent.glass_blur_sigma() == 0.0f,
          "negative sigma clamps to 0");
    check(!extent.set_glass_blur_sigma(nan), "NaN sigma equals the stored 0");

    // The two scalars share four bytes as 16-bit fixed point; they round up, never down, and they
    // are independent of each other.
    check(extent.set_glass_blur_sigma(5.0f) && extent.glass_blur_sigma() == 5.0f &&
              !extent.set_glass_blur_sigma(5.0f),
          "a whole sigma step round-trips exactly and a repeat is no change");
    check(extent.set_glass_blur_sigma(0.001f) && extent.glass_blur_sigma() >= 0.001f,
          "a tiny sigma never rounds down to 0");
    check(extent.set_glass_blur_sigma(1.0e9f) && extent.glass_blur_sigma() > 1000.0f,
          "an absurd sigma saturates instead of wrapping");
    check(extent.filter_reach() == 0.0f, "the reach starts at 0 and the sigma does not set it");
    check(extent.set_filter_reach(12.0f) && extent.filter_reach() == 12.0f &&
              !extent.set_filter_reach(12.0f),
          "a whole reach round-trips exactly and a repeat is no change");
    check(extent.glass_blur_sigma() > 1000.0f, "setting the reach leaves the sigma alone");
    check(extent.set_filter_reach(4.3f) && extent.filter_reach() >= 4.3f &&
              extent.filter_reach() < 4.6f,
          "a fractional reach rounds up to the next step");
    check(extent.set_filter_reach(1.0e9f) && extent.filter_reach() > 16000.0f,
          "an absurd reach saturates");
    check(extent.set_filter_reach(-3.0f) && extent.filter_reach() == 0.0f &&
              !extent.set_filter_reach(nan),
          "negative and NaN reaches count as 0");
    extent.set_glass_blur_sigma(0.0f);
    check(extent.set_outsets(PaintOutsets{-1.0f, 2.0f, nan, 4.0f, false}), "outsets change");
    check(extent.outsets().left == 0.0f && extent.outsets().right == 0.0f &&
              extent.outsets().top == 2.0f,
          "outsets sanitize");
}

} // namespace

int main() {
    hints_are_independent_bits();
    input_policy_reports_changes();
    scroll_style_reports_changes();
    placement_tracks_identity();
    paint_extent_sanitizes_and_caches();
    return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
