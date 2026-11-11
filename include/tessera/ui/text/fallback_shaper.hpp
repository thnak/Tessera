// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace tessera::ui::text {

// Shapes text over an ordered font stack (docs/design/text.md section 4, issue #12). The text is
// cut into clusters (a base code point plus the combining marks, variation selectors, emoji
// modifiers, ZWJ sequences, tag sequences and regional-indicator pairs that belong to it), each
// cluster is given to the first face of the stack that covers all of it, and adjacent clusters of
// the same face are shaped together. A cluster no face covers stays on the first face and renders
// `.notdef`, which `ShapedRun::missing_glyphs` counts: the stack never silently changes between
// runs, and the result depends only on the stack order and the text.
//
// `Glyph::face` is the stack index, so a renderer rasterizes each glyph with the right face.
// Right-to-left order is preserved: sub-runs are shaped with the direction of the whole text and
// concatenated in visual order. Not thread-safe (it shares the cluster-to-face cache), like the
// faces it owns.
class FallbackShaper final : public TextShaper {
public:
    // At least one valid face; invalid faces are dropped and the order of the rest is kept (so a
    // dropped face shifts the indices of the ones after it).
    explicit FallbackShaper(std::vector<FontFace> stack);
    ~FallbackShaper() override;
    FallbackShaper(FallbackShaper&&) noexcept;
    FallbackShaper& operator=(FallbackShaper&&) noexcept;
    FallbackShaper(const FallbackShaper&) = delete;
    FallbackShaper& operator=(const FallbackShaper&) = delete;

    [[nodiscard]] ShapedRun shape(std::string_view utf8, const TextStyle& style) const override;

    // Index of the face that would draw this cluster (the UTF-8 of one cluster), or `size()` when
    // no face covers it. The decision is cached per cluster text.
    [[nodiscard]] std::size_t face_for_cluster(std::string_view cluster) const;

    [[nodiscard]] std::size_t size() const noexcept {
        return shapers_.size();
    }

    // The cluster-to-face decisions cached so far (bounded; the cache restarts when full).
    [[nodiscard]] std::size_t cached_decisions() const noexcept {
        return decisions_.size();
    }

private:
    std::vector<std::unique_ptr<HarfBuzzShaper>> shapers_;
    mutable std::unordered_map<std::string, std::uint16_t> decisions_;
};

} // namespace tessera::ui::text
