// SPDX-License-Identifier: MIT
//
// Locale-driven text and its dirty classification (issue #195; docs/design/dirty-rendering.md
// section 3). The rule is stated once, here: a metric change is a "text change" by that table's own
// row, so it raises Layout; a same-metric re-translation raises Paint only.

#include "tessera/appfw/i18n_text.hpp"

#include <string>
#include <string_view>

namespace tessera::appfw {
namespace {

bool same_metrics(const ui::text::TextMetrics& left, const ui::text::TextMetrics& right) noexcept {
    return left.width == right.width && left.height == right.height &&
           left.baseline == right.baseline && left.line_count == right.line_count;
}

ui::Dirty classify(bool text_changed, bool metrics_changed, bool direction_changed) noexcept {
    // Direction dominates: it changes the style input (the inherited `direction` seeds every
    // logical inline-axis property), so the style resolver, layout, paint and render all have work.
    if (direction_changed) {
        return ui::Dirty::Style | ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render;
    }
    if (metrics_changed) {
        return ui::Dirty::Layout | ui::Dirty::Paint | ui::Dirty::Render;
    }
    if (text_changed) {
        return ui::Dirty::Paint | ui::Dirty::Render;
    }
    return ui::Dirty::Clean;
}

} // namespace

LocalizedText::LocalizedText(std::string_view message_id, ui::text::TextStyle style,
                             ui::text::TextConstraints constraints)
    : message_id_(message_id),
      style_(style),
      constraints_(constraints) {}

RelayoutResult LocalizedText::relocalize(ui::text::TextMeasureCache& cache,
                                         std::string_view resolved, TextDirection direction) {
    const ui::text::TextMetrics metrics = cache.measure(resolved, style_, constraints_);
    const bool text_changed = resolved != text_;
    const bool metrics_changed = !same_metrics(metrics, metrics_);
    const bool direction_changed = direction != direction_;

    RelayoutResult result;
    result.metrics = metrics;
    result.direction_changed = direction_changed;
    result.text_changed = text_changed;
    result.metrics_changed = metrics_changed && text_changed;
    result.dirty = classify(text_changed, result.metrics_changed, direction_changed);
    if (text_changed) {
        result.previous = text_;
        result.current = std::string(resolved);
    }

    // The previous run has no reader left, and the cache is bounded (`TextMeasureCache`), so drop
    // its entry now instead of letting a locale switch leave every earlier translation resident
    // with its old style. That is the spec's "formatting cache invalidation" (section 5) for the
    // one cache the text stack owns.
    if (text_changed && !text_.empty()) {
        cache.invalidate(text_);
    }
    text_ = std::string(resolved);
    metrics_ = metrics;
    direction_ = direction;
    return result;
}

RelayoutResult LocalizedText::restyle(ui::text::TextMeasureCache& cache,
                                      const ui::text::TextStyle& style,
                                      const ui::text::TextConstraints& constraints) {
    const ui::text::TextMetrics previous_metrics = metrics_;
    style_ = style;
    constraints_ = constraints;
    const ui::text::TextMetrics metrics = cache.measure(text_, style_, constraints_);

    RelayoutResult result;
    result.metrics = metrics;
    result.previous = text_;
    result.current = text_;
    result.metrics_changed = !same_metrics(metrics, previous_metrics);
    // A restyle is a style change by construction, so Layout is only raised when the box actually
    // moved; a letter-spacing tweak that does not change the measured box is still a paint change.
    result.dirty = classify(true, result.metrics_changed, false);
    if (result.dirty == ui::Dirty::Clean) {
        result.dirty = ui::Dirty::Paint | ui::Dirty::Render;
    }
    metrics_ = metrics;
    return result;
}

} // namespace tessera::appfw
