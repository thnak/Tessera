// SPDX-License-Identifier: MIT
//
// Fixtures for the AF8 i18n suite (issue #195). Everything here is explicit data: a
// supported-locale table, a compiled message table, and a text measurer with fixed advances. No
// test in the suite reads an OS locale, calls `setlocale`, loads a font, or touches the file system
// - the resolved locale is a function of the arguments alone, which is the spec's "deterministic
// mode" applied to every run (docs/specs/i18n-v1.md section 1).
//
// The message text is deliberately ASCII-only. The *direction* of a locale comes from the table,
// not from the characters in a string (i18n-v1.md section 8.1: the resolved base direction supplies
// the root `direction`), so an RTL assertion does not need Arabic bytes - and keeping the fixture
// ASCII means the test cannot depend on the compiler's or the terminal's encoding for what it
// asserts.

#pragma once

#include "tessera/appfw/i18n_catalog.hpp"
#include "tessera/appfw/i18n_locale.hpp"
#include "tessera/appfw/i18n_service.hpp"
#include "tessera/appfw/i18n_text.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace i18n_test {

using tessera::appfw::I18nServiceConfig;
using tessera::appfw::LocaleMatch;
using tessera::appfw::LocaleResolution;
using tessera::appfw::LocaleService;
using tessera::appfw::LocaleTable;
using tessera::appfw::LocaleTableConfig;
using tessera::appfw::LocalizedText;
using tessera::appfw::MessageCatalog;
using tessera::appfw::MessageCatalogConfig;
using tessera::appfw::MessageEntry;
using tessera::appfw::RelayoutResult;
using tessera::appfw::SupportedLocale;
using tessera::appfw::TextDirection;
using tessera::ui::UpdateScheduler;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::StubTextMeasurer;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMeasureCache;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

inline int g_failures = 0;

inline bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "appfw_i18n: " << message << "\n";
        ++g_failures;
    }
    return condition;
}

// The supported set: `en` (default), `de` (a language-only bundle, so `de_AT` can exercise the
// language step), `de_AT` (an exact regional bundle), `ar` (RTL) and `zh`/`zh_Hant` (the
// language+script step, which is what distinguishes Hant from Hans). Deliberately small: every
// locale here is referenced by at least one assertion, so an edit that orphans one shows up as a
// failing test rather than as dead data.
inline constexpr SupportedLocale kSupportedLocales[] = {
    {"en", TextDirection::LeftToRight},    {"de", TextDirection::LeftToRight},
    {"de_AT", TextDirection::LeftToRight}, {"ar", TextDirection::RightToLeft},
    {"zh", TextDirection::LeftToRight},    {"zh_Hant", TextDirection::LeftToRight},
};

inline LocaleTableConfig table_config(std::string_view fallback = "en",
                                      std::string_view default_locale = "en") {
    LocaleTableConfig config;
    config.locales = kSupportedLocales;
    config.fallback_locale = fallback;
    config.default_locale = default_locale;
    return config;
}

// A fixture that cannot be built is a bug in this file, not an assertion about the module, so it
// ends the test binary with a message instead of returning a value no assertion could interpret. It
// is deterministic: the fixture data is constant.
[[noreturn]] inline void fixture_failed(std::string_view what, std::string_view detail) {
    std::cerr << "appfw_i18n: fixture failed: " << what << "\n";
    if (!detail.empty()) {
        std::cerr << "  " << detail << "\n";
    }
    std::exit(1);
}

// The fixture tables are valid by construction.
inline LocaleTable make_table(std::string_view fallback = "en",
                              std::string_view default_locale = "en") {
    auto table = LocaleTable::create(table_config(fallback, default_locale));
    if (!table.has_value()) {
        fixture_failed("locale table", table.error().subject);
    }
    return std::move(*table);
}

// The message table. `orders.title` is the acceptance's string: "Orders" (6 bytes) in `en`,
// "Bestellungen" (12) in `de` - twice as wide, so a relayout must follow. `orders.short` is 6 bytes
// in both locales, which is how a test proves that a translation that does not move the box is a
// paint change and not a layout one. `orders.empty` exists only in `en` and `de`, so a `de_AT`
// lookup must fall through the language step.
inline constexpr MessageEntry kMessages[] = {
    {"orders.title", "en", {"Orders"}},
    {"orders.title", "de", {"Bestellungen"}},
    {"orders.title", "de_AT", {"Bestellung"}},
    {"orders.title", "ar", {"Talabat"}},
    {"orders.title", "zh_Hant", {"Dingdan"}},
    {"orders.short", "en", {"Orders"}},
    {"orders.short", "de", {"Auftra"}},
    {"orders.empty", "en", {"No orders"}},
    {"orders.empty", "de", {"Keine Bestellungen"}},
};

inline std::span<const MessageEntry> all_messages() {
    return std::span<const MessageEntry>{kMessages, sizeof(kMessages) / sizeof(kMessages[0])};
}

inline MessageCatalogConfig catalog_config(std::string_view default_locale = "en",
                                           std::size_t diagnostic_capacity = 8) {
    MessageCatalogConfig config;
    config.messages = all_messages();
    config.default_locale = default_locale;
    config.diagnostic_capacity = diagnostic_capacity;
    return config;
}

inline MessageCatalog make_catalog(const LocaleTable& table, std::string_view default_locale = "en",
                                   std::size_t diagnostic_capacity = 8) {
    auto catalog =
        MessageCatalog::create(catalog_config(default_locale, diagnostic_capacity), table);
    if (!catalog.has_value()) {
        fixture_failed("message catalog", catalog.error().subject);
    }
    return std::move(*catalog);
}

// The deterministic measurement stack every relayout assertion uses. `StubTextMeasurer` advances
// `font_size * 0.5` per code point and the stub provider makes the line height `font_size * 1.2`,
// so at the default 16 px a width is `code_points * 8` - an integer a test asserts exactly instead
// of within a tolerance, and the same number in every locale and on every machine.
inline std::shared_ptr<StubTextMeasurer> stub_measurer() {
    return std::make_shared<StubTextMeasurer>(std::make_shared<StubFontMetricsProvider>());
}

inline std::shared_ptr<TextMeasureCache> stub_cache(std::size_t capacity = 64) {
    return std::make_shared<TextMeasureCache>(stub_measurer(), capacity);
}

// --- what the relayout suite asserts, named once ------------------------------------------------
//
// The masks live here so the assertion text and the value cannot drift apart, and so the two TUs
// that assert them (the classification and the end-to-end layout) agree by construction.
inline constexpr tessera::ui::Dirty kPaintOnly =
    tessera::ui::Dirty::Paint | tessera::ui::Dirty::Render;
inline constexpr tessera::ui::Dirty kRelayout =
    tessera::ui::Dirty::Layout | tessera::ui::Dirty::Paint | tessera::ui::Dirty::Render;
inline constexpr tessera::ui::Dirty kFull = tessera::ui::Dirty::Style | tessera::ui::Dirty::Layout |
                                            tessera::ui::Dirty::Paint | tessera::ui::Dirty::Render;

// The two numbers every measured-box assertion is derived from (see `stub_measurer`): an advance is
// 8 px per code point at the default 16 px size, and a line box is 19.2 px.
inline constexpr float kAdvancePerByte = 8.0f;
inline constexpr float kLineHeight = 19.2f;

inline bool near(float left, float right) {
    return std::abs(left - right) <= 0.01f;
}

inline const tessera::ui::layout::LayoutBox*
find_box(const tessera::ui::layout::LayoutResult& result, std::uint64_t id) {
    for (const tessera::ui::layout::LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

// A leaf text node with no intrinsic width: exactly what `layout_engine.cpp` turns into a Yoga node
// with the measure callback, so its box is whatever the text seam returned for it.
inline tessera::ui::layout::LayoutInput text_leaf(std::uint64_t id, std::string text) {
    tessera::ui::layout::LayoutInput input;
    input.id = id;
    input.style.set("display", "block");
    input.style.set("font-size", "16px");
    input.text = std::move(text);
    return input;
}

// The fixed page both layout tests lay out: a 400x300 flex column whose items keep their intrinsic
// width (`align-items: flex-start`), so a text leaf's box is its measured width rather than the
// column's.
inline tessera::ui::layout::LayoutInput text_page(std::string text) {
    tessera::ui::layout::LayoutInput root;
    root.id = 1;
    root.style.set("display", "flex");
    root.style.set("flex-direction", "column");
    root.style.set("align-items", "flex-start");
    root.style.set("width", "400px");
    root.style.set("height", "300px");
    root.children.push_back(text_leaf(2, std::move(text)));
    return root;
}

// One whole stack: scheduler, table, catalog, service, declared in dependency order so the service
// is destroyed before the scheduler whose ticket it attached.
struct Harness {
    explicit Harness(std::vector<std::string> initial_requests = {"en"})
        : table(make_table()),
          catalog(make_catalog(table)),
          service(I18nServiceConfig{table, std::move(catalog), std::move(initial_requests)},
                  scheduler) {}

    UpdateScheduler scheduler;
    LocaleTable table;
    MessageCatalog catalog;
    LocaleService service;
};

struct ChangeRecorder {
    static void on_change(void* context, const tessera::appfw::LocaleSnapshot& snapshot) {
        auto* self = static_cast<ChangeRecorder*>(context);
        ++self->calls;
        self->last_tag = snapshot.tag;
        self->last_epoch = snapshot.epoch;
        self->direction = snapshot.direction;
    }

    int calls = 0;
    std::string last_tag;
    std::uint64_t last_epoch = 0;
    TextDirection direction = TextDirection::LeftToRight;
};

} // namespace i18n_test
