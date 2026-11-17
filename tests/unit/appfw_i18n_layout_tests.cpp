// SPDX-License-Identifier: MIT
//
// The acceptance for issue #195, second half: the *measured box*, produced by the real
// `LayoutEngine` rather than reported by a helper.
//
// The first half (`appfw_i18n_relayout_tests.cpp`) proves the dirty mask is computed from a
// measurement. This half proves the measurement is the one Yoga will use: the text leaf has no
// intrinsic width, so its box is whatever the `TextMeasureCache` returned through
// `layout_engine.cpp`'s measure callback. A locale switch that only raised Paint would leave the
// old box here, which is the "relayout bug" the issue names.
//
// It also covers the incremental path the frame pipeline actually drives: `LayoutScheduler` reuses
// its cached layout only while the signature is unchanged, and the translated text is part of that
// signature, so a locale change reflows instead of serving the previous locale's boxes.
//
// Determinism: `StubTextMeasurer` with `StubFontMetricsProvider`. A width is `code_points * 8` at
// the fixture's 16 px, so each assertion is an exact number and the same on every machine.

#include "appfw_i18n_test_support.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/layout/layout_signature.hpp"

#include <string>
#include <string_view>

namespace i18n_test {
namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::layout::layout_signature;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutScheduler;
using tessera::ui::layout::LayoutViewport;

const LayoutViewport kViewport{400.0f, 300.0f};

// English "Orders" (6 code points) and German "Bestellungen" (12) through the real engine: the box
// is the measured width, so the German box is exactly twice as wide.
void the_layout_engine_produces_the_new_measurement() {
    const LayoutEngine engine(stub_measurer(), 64);

    const LayoutResult english = engine.layout(text_page("Orders"), kViewport);
    const LayoutBox* english_box = find_box(english, 2);
    expect(english_box != nullptr, "the English text node has a box");
    expect(english_box != nullptr && near(english_box->bounds.width, 48.0f),
           "Yoga measures Orders at 48 px");
    // Layout is unrounded (paint snaps the edges), so the box keeps the measured 19.2 px line box.
    expect(english_box != nullptr && near(english_box->bounds.height, kLineHeight),
           "layout keeps the 19.2 px line box unrounded");
    auto seam_cache = stub_cache();
    const TextMetrics measured = seam_cache->measure("Orders", TextStyle{}, TextConstraints{});
    expect(near(measured.height, kLineHeight), "the text seam measures the line box at 19.2 px");

    // The locale change: the same node, the new translation, nothing else touched.
    const LayoutResult german = engine.layout(text_page("Bestellungen"), kViewport);
    const LayoutBox* german_box = find_box(german, 2);
    expect(german_box != nullptr, "the German text node has a box");
    expect(german_box != nullptr && near(german_box->bounds.width, 96.0f),
           "Yoga measures Bestellungen at 96 px");
    expect(german_box != nullptr && near(german_box->bounds.height, kLineHeight),
           "Bestellungen is still one line");
    expect(english_box != nullptr && german_box != nullptr &&
               german_box->bounds.width > english_box->bounds.width,
           "the German box is wider than the English box");

    // The text is a layout input, so the signature moves with it: a scheduler keyed on the
    // signature cannot serve the English result for the German text.
    expect(layout_signature(text_page("Orders"), kViewport) !=
               layout_signature(text_page("Bestellungen"), kViewport),
           "the layout signature changes with the translated text");
}

// The whole acceptance sentence in one frame: the service resolves the locale, the binding measures
// through the layout engine's own cache, and the engine lays out the box that measurement implies.
void a_locale_switch_through_the_service_relayouts() {
    Harness harness({"en"});
    LayoutEngine engine(stub_measurer(), 64);
    const std::shared_ptr<TextMeasureCache> cache = engine.text_cache();

    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});

    // Mount: the English text.
    const RelayoutResult mounted =
        slot.relocalize(*cache, harness.service.text("orders.title"), harness.service.direction());
    expect(has_flag(mounted.dirty, Dirty::Layout), "the first measure raises Layout");
    const LayoutResult english = engine.layout(text_page(std::string(mounted.current)), kViewport);
    const LayoutBox* english_box = find_box(english, 2);
    expect(english_box != nullptr && near(english_box->bounds.width, 48.0f),
           "the mounted box is 48 px wide");

    // The user switches the app to German.
    expect(harness.service.set_requests({"de"}), "the locale switch reports a change");
    const std::string_view german_text = harness.service.text("orders.title");
    expect(german_text == "Bestellungen", "the service serves the German pattern");

    const RelayoutResult changed =
        slot.relocalize(*cache, german_text, harness.service.direction());
    expect(changed.dirty == kRelayout,
           "the locale switch raises Layout | Paint | Render for a wider translation");
    expect(near(changed.metrics.width, 96.0f), "the new measurement is 96 px");

    const LayoutResult german = engine.layout(text_page(std::string(changed.current)), kViewport);
    const LayoutBox* german_box = find_box(german, 2);
    expect(german_box != nullptr && near(german_box->bounds.width, 96.0f),
           "the relaid-out box is 96 px wide, matching the binding's measurement");
    expect(english_box != nullptr && german_box != nullptr &&
               near(german_box->bounds.width, changed.metrics.width),
           "the laid-out width equals the measured width the mask was computed from");
}

// The incremental path the frame pipeline drives: the scheduler's cache hit is keyed on the layout
// signature, and the translated text is part of it. The English layout is reused while the tree is
// unchanged and recomputed once the text changes.
void the_layout_scheduler_reflows_for_the_new_locale() {
    LayoutEngine engine(stub_measurer(), 64);
    LayoutScheduler scheduler(engine);

    const auto& english = scheduler.update(text_page("Orders"), kViewport, true);
    expect(scheduler.layout_runs() == 1, "the first update reflows");
    const LayoutBox* english_box = find_box(english, 2);
    expect(english_box != nullptr && near(english_box->bounds.width, 48.0f),
           "the first layout is the English 48 px");

    (void)scheduler.update(text_page("Orders"), kViewport, false);
    expect(scheduler.cache_hits() == 1, "an unchanged tree reuses the cached layout");

    const LayoutResult& reflowed = scheduler.update(text_page("Bestellungen"), kViewport, true);
    expect(scheduler.layout_runs() == 2, "the translated text forces a reflow, not a cache hit");
    const LayoutBox* reflowed_box = find_box(reflowed, 2);
    expect(reflowed_box != nullptr && near(reflowed_box->bounds.width, 96.0f),
           "the scheduler's reflowed box is the German 96 px");
}

// A direction flip is laid out by the engine as mirrored boxes, which is why the mask includes
// Style and Layout: `direction` is inherited style, and Yoga resolves the logical inline-axis edges
// against it (stylesheet-v1 section 11.1, already asserted end to end by `logical_layout`).
void a_direction_flip_reaches_layout() {
    auto cache = stub_cache();
    LocalizedText slot("orders.title", TextStyle{}, TextConstraints{});
    const RelayoutResult ltr = slot.relocalize(*cache, "Talabat", TextDirection::LeftToRight);
    const RelayoutResult rtl = slot.relocalize(*cache, "Talabat", TextDirection::RightToLeft);
    expect(rtl.dirty == kFull, "the flip is Style | Layout | Paint | Render");
    expect(near(ltr.metrics.width, rtl.metrics.width),
           "the same text measures the same width in both directions");
    expect(ltr.metrics_changed || !ltr.text_changed, "the first call had no previous text");
}

} // namespace

int run_layout_tests() {
    const int before = g_failures;
    the_layout_engine_produces_the_new_measurement();
    a_locale_switch_through_the_service_relayouts();
    the_layout_scheduler_reflows_for_the_new_locale();
    a_direction_flip_reaches_layout();
    return g_failures - before;
}

} // namespace i18n_test
