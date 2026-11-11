// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::ui::text {

// One glyph atlas per distinct quantized render scale.
//
// Why: text is rasterized at the window's device scale. A bitmap produced for 1.25 must never be
// sampled for 2.0 (it would be blurry) nor the other way round (it would alias), so the scale is
// part of the cache identity instead of a per-call pixel-size convention. One atlas per ladder step
// also turns a DPI change into a single, bounded eviction rather than a flush of every glyph.
//
// Invariants:
// - Slots are bounded by kRenderScaleStepCount (4), one atlas each, and the key is always the
//   quantized scale: `atlas_for(1.0833)` and `atlas_for(1.0)` are the same atlas, 3.0 clamps to the
//   2.0 slot, and a non-finite scale quantizes to the ladder floor. No call sequence grows the set.
// - Nothing is released for capacity. An atlas leaves the set only through invalidate() (the
//   previous step after a DPI change) or invalidate_all() (device loss/shutdown), so a window that
//   bounces between two monitors keeps re-using both atlases instead of re-rasterizing on every
//   move.
// - Each atlas owns its page LRU and generation counters, so page eviction on one scale never
//   invalidates another scale's pages.
class ScaleGlyphAtlas {
public:
    ScaleGlyphAtlas(GlyphRasterizer& rasterizer, GlyphAtlasConfig config = {});
    // A font stack: every atlas in the set rasterizes glyph `face` i with `rasterizers[i]`, so a
    // face-aware renderer keeps glyphs of every face in the same per-step atlas (and page). The
    // rasterizers must outlive the set.
    ScaleGlyphAtlas(std::vector<GlyphRasterizer*> rasterizers, GlyphAtlasConfig config = {});

    ScaleGlyphAtlas(const ScaleGlyphAtlas&) = delete;
    ScaleGlyphAtlas& operator=(const ScaleGlyphAtlas&) = delete;
    ScaleGlyphAtlas(ScaleGlyphAtlas&&) noexcept = default;
    ScaleGlyphAtlas& operator=(ScaleGlyphAtlas&&) noexcept = default;

    // Atlas owning `render_scale` (quantized on read); created on first use. References stay valid
    // for the life of the set (slots are fixed), so a renderer may cache them per frame.
    GlyphAtlas& atlas_for(float render_scale);
    [[nodiscard]] GlyphAtlas* find_atlas(float render_scale) noexcept;
    [[nodiscard]] const GlyphAtlas* find_atlas(float render_scale) const noexcept;

    // Advances the page-LRU frame counter of every live atlas. Call once per frame before drawing
    // so a page used on any scale is never evicted in the frame that used it.
    void begin_frame();

    // Drops the atlas for `render_scale` (quantized) so the next get() re-rasterizes every glyph at
    // that scale. Returns whether an atlas was released. This is the DPI-change contract: call it
    // for the *previous* step, then draw at the new step; unaffected scales keep their pages.
    bool invalidate(float render_scale);
    void invalidate_all();

    [[nodiscard]] std::uint32_t atlas_count() const noexcept;
    // Atlas instances created since construction. It counts instances, not live slots, so
    // re-rasterization after an invalidate() shows up here for diagnostics.
    [[nodiscard]] std::uint64_t created_count() const noexcept;
    [[nodiscard]] std::uint64_t invalidated_count() const noexcept;

private:
    [[nodiscard]] static std::size_t slot_for(float render_scale) noexcept;

    std::vector<GlyphRasterizer*> rasterizers_;
    GlyphAtlasConfig config_;
    std::array<std::unique_ptr<GlyphAtlas>, scale::kRenderScaleStepCount> slots_;
    std::uint64_t created_count_ = 0;
    std::uint64_t invalidated_count_ = 0;
};

} // namespace tessera::ui::text
