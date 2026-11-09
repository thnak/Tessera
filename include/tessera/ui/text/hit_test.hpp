// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_layout.hpp"

#include <cstddef>
#include <string_view>

namespace tessera::ui::text {

// Caret affinity at an ambiguous point: Downstream places the caret before the following cluster
// (leading half of a glyph / the run that starts at a join), Upstream places it after the preceding
// cluster. Ties choose Downstream (right-biased), matching the paint join-affinity rule.
enum class CaretAffinity { Upstream, Downstream };

struct TextHit {
    std::size_t byte_offset = 0;
    CaretAffinity affinity = CaretAffinity::Downstream;

    bool operator==(const TextHit&) const = default;
};

// Maps a local point (x from the content-box left, y from its top) to a logical byte caret.
// Lines are selected by y (clamped above/below to the first/last line), then x is resolved inside
// that line's visual runs (bidi-aware: RTL run logical order runs from the visual right edge). The
// result snaps to a grapheme-cluster boundary. `measure` defaults to the deterministic stub based
// on `metrics`; pass the real advance source to match a real-face paint. Alignment is Start: the
// caller subtracts any centered/end line offset before calling.
[[nodiscard]] TextHit hit_test_point(std::string_view text, const TextStyle& style,
                                     const TextConstraints& constraints, float x, float y,
                                     const FontMetrics& metrics,
                                     TextAdvanceMeasure measure = nullptr, void* user = nullptr);

} // namespace tessera::ui::text
