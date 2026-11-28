// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::ui::text {

// One byte range of a paragraph that is measured and painted at a fraction of the paragraph's font
// size: the metric half of an inline span (`sub`, `sup`, `small`; docs/design/inline-size-runs.md).
// Offsets address the text handed to the measurer, so they are the same bytes a PaintSpan holds.
struct TextSizeRun {
    std::uint32_t begin = 0;
    std::uint32_t end = 0;
    float scale = 1.0f;

    friend constexpr bool operator==(const TextSizeRun&, const TextSizeRun&) = default;
};

// A process-wide handle to an immutable run table, so a TextStyle carries 4 bytes instead of the
// table and every cache key can compare it exactly (the FontFeaturesId pattern). 0 means "no runs":
// the whole paragraph is measured at TextStyle::font_size. Equal tables share one id. The table
// holds at most kMaxInternedTextSizeRuns distinct tables; past that a new table interns as 0 (the
// spans keep the paragraph size) instead of growing without bound. Interning is thread-safe.
struct TextSizeRunsId {
    std::uint32_t value = 0;

    friend constexpr bool operator==(TextSizeRunsId, TextSizeRunsId) = default;
    friend constexpr auto operator<=>(TextSizeRunsId, TextSizeRunsId) = default;
};

inline constexpr std::size_t kMaxInternedTextSizeRuns = 256;
inline constexpr float kMinTextSizeRunScale = 0.25f;

// The runs must be sorted and disjoint. A run whose scale is not below 1 (or is not finite) is
// dropped: V1 spans may shrink but not grow, because the line box stays the paragraph's own.
// Returns 0 when nothing is left.
[[nodiscard]] TextSizeRunsId intern_text_size_runs(std::span<const TextSizeRun> runs);

// The table behind an id, stable for the life of the process. An unknown id (including 0) resolves
// to an empty span.
[[nodiscard]] std::span<const TextSizeRun> resolve_text_size_runs(TextSizeRunsId id);

// An inline span's resolved font size over the byte range it covers. The layout pass and the paint
// pass both build their table from this, so the two measure the same advances.
struct SpanFontSize {
    std::size_t begin = 0;
    std::size_t end = 0;
    float font_size = 0.0f;
};

// Interns the runs of the spans whose font size is below the paragraph's `parent_font_size`. The
// spans must be in source order.
[[nodiscard]] TextSizeRunsId size_runs_from_spans(std::span<const SpanFontSize> spans,
                                                  float parent_font_size);

// The scale in effect at `offset` (1 outside every run).
[[nodiscard]] float text_size_run_scale(std::span<const TextSizeRun> runs, std::size_t offset);

} // namespace tessera::ui::text
