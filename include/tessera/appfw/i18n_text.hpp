// SPDX-License-Identifier: MIT
//
// Locale-driven text: the half of i18n that decides *what a locale change dirties* (issue #195
// acceptance: "locale change updates text without relayout bugs"; docs/specs/i18n-v1.md sections 4,
// 5 and 7; docs/design/dirty-rendering.md section 3).
//
// ## Why this type exists
//
// In the dirty model (`include/tessera/ui/dirty.hpp`) there are two very different ways text can
// change, and a locale switch produces both:
//
//   * a translation whose measured box is **identical** to the old one (a re-wording, a same-width
//     string) changes pixels only: `Dirty::Paint | Dirty::Render`. Asking Yoga to reflow for it is
//     wasted work, and `docs/design/dirty-rendering.md` section 3 assigns a paint-only change
//     exactly this mask;
//   * a translation whose measured box **differs** is the "text/font change" row of that table:
//     `Dirty::Layout | Dirty::Paint | Dirty::Render`. This is the case the acceptance is about. A
//     wider German string that was filed under `Paint` only would paint at the old box, and the
//     renderer would clip or overlap it - the "relayout bug" the issue names.
//
// A direction flip is a third case: the resolved base direction seeds the root's inherited
// `direction` property (i18n-v1.md section 4), so the *style* input changed, and with it every
// logical inline-axis property downstream: `Dirty::Style | Dirty::Layout | Dirty::Paint |
// Dirty::Render`. Saying "Paint" there would leave an RTL app LTR until something else reflowed it.
//
// So `LocalizedText::relocalize` returns a mask it computed from a measurement, not a constant. The
// caller marks its node with the result; nothing here touches a scene node (services.md section 1:
// "Services never touch scene nodes directly; the app maps results to state").
//
// ## Determinism
//
// Measurement goes through the `ui::text::TextMeasureCache` the caller supplies, i.e. the same seam
// layout uses (`src/ui/layout/layout_engine.cpp` measure callback), so what this reports is what
// Yoga will see. A test supplies `ui::text::StubTextMeasurer` with an explicit font-metrics
// provider: fixed advances, no font file, no system font, no locale-dependent shaping. Nothing here
// reads a locale, a clock, or the file system.

#pragma once

#include "tessera/appfw/i18n_locale.hpp"
#include "tessera/ui/dirty.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_measure_cache.hpp"

#include <string>
#include <string_view>

namespace tessera::appfw {

// What changed for one text run when the locale changed. The mask is the contract; the metrics are
// the evidence a test asserts (the acceptance requires the *measured size*, not just a non-Clean
// mask).
//
// `previous` and `current` own their bytes rather than borrowing: the previous text is about to be
// replaced inside `LocalizedText`, and a caller that logs a diagnostic after the call must not hold
// a dangling view. Both are only written when the text actually changed, so a same-locale call does
// no string work.
struct RelayoutResult {
    // Mask the caller should hand to `Node::mark_dirty`. `Dirty::Clean` means "nothing changed":
    // the same locale was set again, or two catalogs happen to agree.
    ui::Dirty dirty = ui::Dirty::Clean;
    // The measurement of the new text. Always the current one, even when nothing changed.
    ui::text::TextMetrics metrics{};
    std::string previous;
    std::string current;
    // True when only the direction changed (the text and its box did not). Useful for a caller that
    // mirrors icons, which the spec keeps separate from layout (i18n-v1.md section 8.1, RTL/icon
    // policy).
    bool direction_changed = false;
    bool text_changed = false;
    bool metrics_changed = false;
};

// One localized text slot: a message id, the style/constraints it is laid out with, and the text
// and metrics that were current as of the last call. It is a value a component keeps per string.
class LocalizedText {
public:
    LocalizedText(std::string_view message_id, ui::text::TextStyle style,
                  ui::text::TextConstraints constraints);

    // The acceptance path. `resolved` is what `LocaleService::text(id)` returned; `cache` must be
    // the cache layout uses, so the new box is the box Yoga will produce; `direction` is
    // `LocaleService::direction()`.
    RelayoutResult relocalize(ui::text::TextMeasureCache& cache, std::string_view resolved,
                              TextDirection direction);

    // A style change (not a locale change) re-measures with the same text. It returns a mask by the
    // same rule, so a caller does not need a second classifier. A locale-driven restyle should use
    // `relocalize`.
    RelayoutResult restyle(ui::text::TextMeasureCache& cache, const ui::text::TextStyle& style,
                           const ui::text::TextConstraints& constraints);

    [[nodiscard]] std::string_view message_id() const noexcept {
        return message_id_;
    }
    [[nodiscard]] std::string_view text() const noexcept {
        return text_;
    }
    [[nodiscard]] const ui::text::TextMetrics& metrics() const noexcept {
        return metrics_;
    }
    [[nodiscard]] TextDirection direction() const noexcept {
        return direction_;
    }
    [[nodiscard]] const ui::text::TextStyle& style() const noexcept {
        return style_;
    }
    [[nodiscard]] const ui::text::TextConstraints& constraints() const noexcept {
        return constraints_;
    }

private:
    std::string message_id_;
    ui::text::TextStyle style_;
    ui::text::TextConstraints constraints_;
    std::string text_;
    ui::text::TextMetrics metrics_{};
    TextDirection direction_ = TextDirection::LeftToRight;
};

} // namespace tessera::appfw
