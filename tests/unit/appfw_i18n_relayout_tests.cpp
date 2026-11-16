// SPDX-License-Identifier: MIT
//
// The acceptance for issue #195, first half: *which* dirty domains a locale change raises.
//
// A mask is not enough on its own (the second half, `appfw_i18n_layout_tests.cpp`, lays the same
// text out through the real `LayoutEngine`), but it is the half that decides whether layout runs at
// all, and it is computed from a measurement rather than assumed:
//
//   * a wider translation   -> `Layout | Paint | Render`
//   * a same-width one      -> `Paint | Render` only
//   * a direction flip      -> `Style | Layout | Paint | Render`
//   * the same locale again -> `Clean`
//
// Determinism: `StubTextMeasurer` with `StubFontMetricsProvider`, so an advance is exactly 8 px per
// code point at the fixture's 16 px. No font file, no system font, no clock, no OS locale.

#include "appfw_i18n_test_support.hpp"

#include <string>
#include <string_view>

namespace i18n_test {
namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;

// A wider translation is the case the acceptance names: filed under Paint only it would paint at
// the old box and clip.
void a_wider_translation_raises_layout() {
    auto cache = stub_cache();
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});

    // Install the English text, the state a mounted component is in. Nothing was measured before,
    // so the first call is a layout change too.
    const RelayoutResult first = slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);
    expect(first.dirty == kRelayout,
           "the first text is a layout change (nothing was measured before)");
    expect(near(first.metrics.width, 6.0f * kAdvancePerByte), "Orders measures 48 px");
    expect(near(first.metrics.height, kLineHeight), "Orders is one 19.2 px line");

    // The locale change. "Bestellungen" is 12 code points, twice "Orders".
    const RelayoutResult relayout =
        slot.relocalize(*cache, "Bestellungen", TextDirection::LeftToRight);
    expect(relayout.dirty == kRelayout, "a wider translation raises Layout | Paint | Render");
    expect(has_flag(relayout.dirty, Dirty::Layout), "Layout is in the mask");
    expect(relayout.text_changed, "the text changed");
    expect(relayout.metrics_changed, "the measured box changed");
    expect(!relayout.direction_changed, "the direction did not change");
    expect(near(relayout.metrics.width, 12.0f * kAdvancePerByte),
           "Bestellungen measures 96 px, exactly twice Orders");
    expect(relayout.previous == "Orders", "the previous text is reported");
    expect(relayout.current == "Bestellungen", "the new text is reported");
}

// The other half of the same rule: a translation that does not move the box must not relayout. This
// is why the mask is computed instead of constant.
void a_same_width_translation_is_paint_only() {
    auto cache = stub_cache();
    LocalizedText slot("orders.short", TextStyle{}, TextConstraints{});
    const RelayoutResult first = slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);
    expect(near(first.metrics.width, 48.0f), "Orders measures 48 px");

    const RelayoutResult relayout = slot.relocalize(*cache, "Auftra", TextDirection::LeftToRight);
    expect(relayout.dirty == kPaintOnly, "a same-width translation is Paint | Render, not Layout");
    expect(!has_flag(relayout.dirty, Dirty::Layout), "Layout is not in the mask");
    expect(relayout.text_changed, "the text did change");
    expect(!relayout.metrics_changed, "the measured box did not change");
    expect(near(relayout.metrics.width, 48.0f), "the width is unchanged at 48 px");
}

// A direction flip changes the style input the inherited `direction` property reads (i18n-v1.md
// section 4), so it is the widest of the three cases. Saying "Paint" here would leave an RTL app
// LTR until something else happened to reflow it.
void a_direction_flip_raises_style_and_layout() {
    auto cache = stub_cache();
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});
    (void)slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);

    const RelayoutResult relayout = slot.relocalize(*cache, "Talabat", TextDirection::RightToLeft);
    expect(relayout.dirty == kFull, "a direction flip raises Style | Layout | Paint | Render");
    expect(relayout.direction_changed, "the direction change is reported");
    expect(has_flag(relayout.dirty, Dirty::Style),
           "Style is in the mask: direction seeds an inherited style property");
}

// Re-publishing the current locale is not a change. A "clean" mask is as load-bearing as the
// others: a host that forwards a settings panel's keystrokes must not relayout per key.
void an_unchanged_locale_is_clean() {
    auto cache = stub_cache();
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});
    (void)slot.relocalize(*cache, "Bestellungen", TextDirection::LeftToRight);
    const RelayoutResult again =
        slot.relocalize(*cache, "Bestellungen", TextDirection::LeftToRight);
    expect(again.dirty == Dirty::Clean, "the same locale and text is Clean");
    expect(!again.text_changed, "the text is unchanged");
    expect(!again.metrics_changed, "the box is unchanged");
    expect(near(again.metrics.width, 96.0f), "the measurement is still reported");
}

// A wider translation that crosses the wrap constraint gains a line, so the height changes too. The
// mask is the same, but the evidence is a second line: the case where painting at the old height
// clips rather than merely mis-measures.
void a_wider_translation_that_wraps_grows_the_box() {
    auto cache = stub_cache();
    TextConstraints constraints;
    constraints.max_width = 80.0f; // 10 code points per line at 8 px each
    LocalizedText slot("orders.title", TextStyle{}, constraints);
    (void)slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);

    const RelayoutResult relayout =
        slot.relocalize(*cache, "Bestellungen", TextDirection::LeftToRight);
    expect(relayout.dirty == kRelayout, "a wrapping translation still raises Layout");
    expect(near(relayout.metrics.width, 80.0f), "the widest line fills the constraint");
    expect(relayout.metrics.line_count == 2, "Bestellungen wraps to two lines at 80 px");
    expect(near(relayout.metrics.height, 2.0f * kLineHeight), "the box grows to two lines");
    expect(relayout.metrics.height > kLineHeight,
           "the height change is what a paint-only pass clips");
}

// The classification is a function of the measurement, so two independently built stacks agree. If
// it depended on anything ambient (a locale, a font, a clock) this is where it would show.
void measurement_is_reproducible_across_stacks() {
    const auto first_cache = stub_cache();
    const auto second_cache = stub_cache();
    LocalizedText first("orders.title", TextStyle{}, TextConstraints{});
    LocalizedText second("orders.title", TextStyle{}, TextConstraints{});

    const RelayoutResult a =
        first.relocalize(*first_cache, "Bestellungen", TextDirection::LeftToRight);
    const RelayoutResult b =
        second.relocalize(*second_cache, "Bestellungen", TextDirection::LeftToRight);
    expect(a.dirty == b.dirty, "two stacks classify the same text identically");
    expect(a.metrics.width == b.metrics.width, "two stacks measure the same width");
    expect(a.metrics.height == b.metrics.height, "two stacks measure the same height");
    expect(a.metrics.line_count == b.metrics.line_count, "two stacks wrap identically");
}

// A style change is not a locale change, but it needs the same classification: a font-size change
// that moves the box must raise Layout, and the text must not be touched.
void a_style_change_re_measures_without_a_locale_change() {
    auto cache = stub_cache();
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});
    (void)slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);

    TextStyle larger;
    larger.font_size = 32.0f;
    const RelayoutResult restyled = slot.restyle(*cache, larger, TextConstraints{});
    expect(has_flag(restyled.dirty, Dirty::Layout), "a font-size change raises Layout");
    expect(near(restyled.metrics.width, 6.0f * 16.0f), "at 32 px, Orders measures 96 px");
    expect(near(restyled.metrics.height, 32.0f * 1.2f), "the line height follows the font size");
    expect(restyled.current == restyled.previous, "a restyle does not change the text");
}

// The old run's cache entry is dropped on a locale change: the spec's "formatting cache
// invalidation" (section 5) for the one cache the text stack owns, and the reason the bounded cache
// does not fill with every translation an app has ever shown.
void a_locale_change_evicts_the_previous_measurement() {
    auto cache = stub_cache(4);
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});
    (void)slot.relocalize(*cache, "Orders", TextDirection::LeftToRight);
    expect(cache->size() == 1, "the English measurement is cached");

    (void)slot.relocalize(*cache, "Bestellungen", TextDirection::LeftToRight);
    const auto stats = cache->stats();
    expect(stats.invalidations == 1, "the previous translation's entry was invalidated");
    expect(cache->size() == 1, "only the current translation is resident");
}

} // namespace

int run_relayout_tests() {
    const int before = g_failures;
    a_wider_translation_raises_layout();
    a_same_width_translation_is_paint_only();
    a_direction_flip_raises_style_and_layout();
    an_unchanged_locale_is_clean();
    a_wider_translation_that_wraps_grows_the_box();
    measurement_is_reproducible_across_stacks();
    a_style_change_re_measures_without_a_locale_change();
    a_locale_change_evicts_the_previous_measurement();
    return g_failures - before;
}

} // namespace i18n_test
