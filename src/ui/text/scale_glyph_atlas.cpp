// SPDX-License-Identifier: MIT

#include "tessera/ui/text/scale_glyph_atlas.hpp"

#include <cstddef>
#include <utility>

namespace tessera::ui::text {

ScaleGlyphAtlas::ScaleGlyphAtlas(GlyphRasterizer& rasterizer, GlyphAtlasConfig config)
    : ScaleGlyphAtlas(std::vector<GlyphRasterizer*>{&rasterizer}, config) {}

ScaleGlyphAtlas::ScaleGlyphAtlas(std::vector<GlyphRasterizer*> rasterizers, GlyphAtlasConfig config)
    : rasterizers_(std::move(rasterizers)),
      config_(config) {}

std::size_t ScaleGlyphAtlas::slot_for(float render_scale) noexcept {
    // quantize_render_scale is total, so the index is always one of the ladder slots; the negative
    // guard only keeps the cast well-defined if the ladder ever loses its floor value.
    const std::int32_t index =
        scale::render_scale_index(scale::quantize_render_scale(render_scale));
    return static_cast<std::size_t>(index > 0 ? index : 0);
}

GlyphAtlas& ScaleGlyphAtlas::atlas_for(float render_scale) {
    std::unique_ptr<GlyphAtlas>& slot = slots_[slot_for(render_scale)];
    if (!slot) {
        slot = std::make_unique<GlyphAtlas>(rasterizers_, config_);
        ++created_count_;
    }
    return *slot;
}

GlyphAtlas* ScaleGlyphAtlas::find_atlas(float render_scale) noexcept {
    return slots_[slot_for(render_scale)].get();
}

const GlyphAtlas* ScaleGlyphAtlas::find_atlas(float render_scale) const noexcept {
    return slots_[slot_for(render_scale)].get();
}

void ScaleGlyphAtlas::begin_frame() {
    for (std::unique_ptr<GlyphAtlas>& slot : slots_) {
        if (slot) {
            slot->begin_frame();
        }
    }
}

bool ScaleGlyphAtlas::invalidate(float render_scale) {
    std::unique_ptr<GlyphAtlas>& slot = slots_[slot_for(render_scale)];
    if (!slot) {
        return false;
    }
    slot.reset();
    ++invalidated_count_;
    return true;
}

void ScaleGlyphAtlas::invalidate_all() {
    for (std::unique_ptr<GlyphAtlas>& slot : slots_) {
        if (slot) {
            slot.reset();
            ++invalidated_count_;
        }
    }
}

std::uint32_t ScaleGlyphAtlas::atlas_count() const noexcept {
    std::uint32_t count = 0;
    for (const std::unique_ptr<GlyphAtlas>& slot : slots_) {
        count += slot ? 1u : 0u;
    }
    return count;
}

std::uint64_t ScaleGlyphAtlas::created_count() const noexcept {
    return created_count_;
}

std::uint64_t ScaleGlyphAtlas::invalidated_count() const noexcept {
    return invalidated_count_;
}

} // namespace tessera::ui::text
