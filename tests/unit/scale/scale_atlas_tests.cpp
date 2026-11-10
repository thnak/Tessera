// SPDX-License-Identifier: MIT

// Per-scale glyph atlas: quantized keying, one atlas per ladder step, bounded slot set, no
// cross-scale glyph reuse, and the DPI-change invalidation/re-rasterization contract.

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using tessera::ui::scale::kRenderScaleLadder;
using tessera::ui::scale::kRenderScaleStepCount;
using tessera::ui::scale::quantize_render_scale;
using tessera::ui::scale::render_scale_index;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasConfig;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::ScaleGlyphAtlas;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scale_atlas: " << message << "\n";
    }
    return condition;
}

GlyphAtlasConfig tight_config() {
    GlyphAtlasConfig config;
    config.page_width = 32;
    config.page_height = 32;
    config.max_pages = 1;
    config.padding = 1;
    return config;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "scale_atlas: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "scale_atlas: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }
    GlyphRasterizer rasterizer(*loaded);

    ScaleGlyphAtlas atlases(rasterizer, tight_config());
    if (!expect(atlases.atlas_count() == 0, "a fresh set has no atlases") ||
        !expect(atlases.created_count() == 0, "a fresh set created nothing") ||
        !expect(atlases.invalidated_count() == 0, "a fresh set invalidated nothing") ||
        !expect(atlases.find_atlas(1.0f) == nullptr, "a fresh set has no 1.0 atlas")) {
        return 2;
    }

    // The key is the quantized scale, so fractional device scales share one atlas.
    GlyphAtlas& one = atlases.atlas_for(1.0f);
    if (!expect(atlases.atlas_count() == 1, "1.0 creates one atlas") ||
        !expect(atlases.created_count() == 1, "1.0 creates once") ||
        !expect(&atlases.atlas_for(1.0833f) == &one, "1.0833 quantizes onto the 1.0 atlas") ||
        !expect(&atlases.atlas_for(0.5f) == &one, "0.5 clamps onto the 1.0 atlas") ||
        !expect(atlases.created_count() == 1, "a quantized hit creates nothing")) {
        return 3;
    }

    // One atlas per distinct ladder step, created lazily and never for an in-between scale.
    GlyphAtlas& two = atlases.atlas_for(3.0f);
    if (!expect(&two != &one, "2.0 is a distinct atlas") ||
        !expect(&atlases.atlas_for(2.0f) == &two, "3.0 clamps onto the 2.0 atlas") ||
        !expect(atlases.atlas_count() == 2, "a second step creates a second atlas")) {
        return 4;
    }
    GlyphAtlas& mid = atlases.atlas_for(1.25f);
    GlyphAtlas& upper = atlases.atlas_for(1.5f);
    if (!expect(&mid != &one && &mid != &two && &upper != &one && &upper != &two && &upper != &mid,
                "every ladder step has its own atlas") ||
        !expect(atlases.atlas_count() == kRenderScaleStepCount,
                "the set holds one atlas per step")) {
        return 5;
    }

    // The slot set is bounded: sweeping every scale in and out of range cannot grow it, and each
    // scale keeps mapping to the same instance.
    const std::array<GlyphAtlas*, 4> expected{&one, &mid, &upper, &two};
    const std::uint64_t created_before_sweep = atlases.created_count();
    for (float render_scale = 0.10f; render_scale <= 4.0f; render_scale += 0.05f) {
        GlyphAtlas& slot = atlases.atlas_for(render_scale);
        const std::int32_t index_in_ladder =
            render_scale_index(quantize_render_scale(render_scale));
        if (!expect(index_in_ladder >= 0 &&
                        static_cast<std::size_t>(index_in_ladder) < expected.size(),
                    "a swept scale quantizes onto the ladder")) {
            return 6;
        }
        if (!expect(&slot == expected[static_cast<std::size_t>(index_in_ladder)],
                    "a swept scale maps to its ladder slot")) {
            return 6;
        }
    }
    if (!expect(atlases.atlas_count() == kRenderScaleStepCount, "the sweep keeps four atlases") ||
        !expect(atlases.created_count() == created_before_sweep, "the sweep creates nothing") ||
        !expect(&atlases.atlas_for(1.0f) == &one, "earlier references survive later slots") ||
        !expect(atlases.find_atlas(1.5f) == &upper, "find_atlas reports the live atlas")) {
        return 7;
    }

    // A non-finite scale is a total input, not a crash: it quantizes to the ladder floor.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    if (!expect(&atlases.atlas_for(nan) == &one, "a NaN scale quantizes to the 1.0 atlas") ||
        !expect(atlases.atlas_count() == kRenderScaleStepCount, "a NaN scale creates nothing")) {
        return 8;
    }

    // begin_frame advances every live atlas and creates none.
    atlases.begin_frame();
    if (!expect(atlases.atlas_count() == kRenderScaleStepCount,
                "begin_frame keeps the atlas count")) {
        return 9;
    }

    // No cross-scale glyph reuse: the same glyph at the same pixel size is cached per scale, and a
    // glyph requested on one scale is invisible to the others.
    const GlyphAtlasEntry low_a = one.get(U'A', 16);
    if (!expect(low_a.has_bitmap, "A rasterizes at 1.0") ||
        !expect(two.find(U'A', 16) == nullptr, "the 2.0 atlas does not inherit 1.0's glyph")) {
        return 10;
    }
    const GlyphAtlasEntry high_a = two.get(U'A', 16);
    if (!expect(high_a.has_bitmap, "A rasterizes at 2.0") ||
        !expect(one.find(U'A', 16) != two.find(U'A', 16), "the two scales keep separate entries") ||
        !expect(one.find(U'A', 16) != nullptr && two.find(U'A', 16) != nullptr,
                "both scales keep their own entry")) {
        return 11;
    }
    const std::uint64_t one_hits = one.stats().hits;
    const std::uint64_t two_hits = two.stats().hits;
    (void)two.get(U'A', 16);
    if (!expect(two.stats().hits == two_hits + 1, "the 2.0 atlas serves its own hit") ||
        !expect(one.stats().hits == one_hits, "the 1.0 atlas is untouched by a 2.0 hit") ||
        !expect(mid.find(U'A', 16) == nullptr && upper.find(U'A', 16) == nullptr,
                "the other scales never see the glyph")) {
        return 12;
    }
    (void)one.get(U'Q', 16);
    if (!expect(one.find(U'Q', 16) != nullptr, "Q rasterizes at 1.0") ||
        !expect(two.find(U'Q', 16) == nullptr, "Q is not shared across scales")) {
        return 13;
    }

    // Invalidation is per scale: the DPI-change contract releases the previous step only.
    const std::uint64_t created_before_invalidate = atlases.created_count();
    const std::uint64_t invalidated_before = atlases.invalidated_count();
    if (!expect(atlases.invalidate(1.24f), "invalidating a live step reports true") ||
        !expect(atlases.atlas_count() == kRenderScaleStepCount - 1, "one step is released") ||
        !expect(atlases.invalidated_count() == invalidated_before + 1,
                "the invalidation is counted") ||
        !expect(atlases.find_atlas(1.25f) == nullptr, "the invalidated step is gone") ||
        !expect(atlases.find_atlas(1.0f) == &one, "the 1.0 atlas survives") ||
        !expect(atlases.find_atlas(2.0f) == &two, "the 2.0 atlas survives") ||
        !expect(!atlases.invalidate(1.25f), "invalidating an absent step reports false")) {
        return 14;
    }

    // Re-rasterization: the next request rebuilds the step with a cold cache. (Addresses are not
    // an identity check here: the allocator is free to hand the released block straight back.)
    GlyphAtlas& rebuilt = atlases.atlas_for(1.25f);
    if (!expect(atlases.created_count() == created_before_invalidate + 1,
                "the rebuild is counted as a creation") ||
        !expect(rebuilt.find(U'A', 16) == nullptr, "the rebuilt atlas starts cold") ||
        !expect(atlases.find_atlas(1.0833f) == &one, "the rebuild did not disturb 1.0")) {
        return 15;
    }
    const std::uint64_t rebuilt_misses = rebuilt.stats().misses;
    (void)rebuilt.get(U'A', 16);
    if (!expect(rebuilt.stats().misses == rebuilt_misses + 1, "the rebuild re-rasterizes") ||
        !expect(rebuilt.find(U'A', 16) != nullptr, "the re-rasterized glyph is cached")) {
        return 16;
    }

    // invalidate_all drops every live atlas (device loss / shutdown).
    const std::uint64_t invalidated_before_all = atlases.invalidated_count();
    const std::uint32_t live_before_all = atlases.atlas_count();
    atlases.invalidate_all();
    if (!expect(atlases.atlas_count() == 0, "invalidate_all drops every atlas") ||
        !expect(atlases.invalidated_count() == invalidated_before_all + live_before_all,
                "invalidate_all counts the live atlases") ||
        !expect(atlases.created_count() == created_before_invalidate + 1,
                "invalidate_all creates nothing")) {
        return 17;
    }
    for (const float render_scale : kRenderScaleLadder) {
        if (!expect(atlases.find_atlas(render_scale) == nullptr,
                    "no ladder step survives invalidate_all")) {
            return 18;
        }
    }

    // Page LRU is per atlas: evicting one scale's single page never invalidates another scale's
    // pages or its cached glyphs.
    ScaleGlyphAtlas churner(rasterizer, tight_config());
    GlyphAtlas& kept = churner.atlas_for(1.0f);
    GlyphAtlas& churned = churner.atlas_for(2.0f);
    churner.begin_frame();
    if (!expect(kept.get(U'A', 16).has_bitmap, "the kept atlas packs a glyph") ||
        !expect(churned.get(U'B', 16).has_bitmap, "the churned atlas packs a glyph")) {
        return 19;
    }
    const std::uint32_t kept_generation = kept.page_generation(0);
    // Fill the churned atlas' only page in this frame; whatever does not fit is dropped, not
    // evicted, because a page touched this frame is never the victim.
    for (char32_t codepoint = U'a'; codepoint <= U'j'; ++codepoint) {
        (void)churned.get(codepoint, 20);
    }
    // On the next frame the page was not touched yet, so a brand-new glyph evicts it.
    const std::uint64_t evictions_before = churned.stats().evictions;
    churner.begin_frame();
    (void)churned.get(U'Z', 20);
    if (!expect(churned.stats().evictions > evictions_before,
                "the churned atlas evicts its single page") ||
        !expect(kept.page_generation(0) == kept_generation,
                "the kept page generation is untouched") ||
        !expect(kept.find(U'A', 16) != nullptr, "the kept glyph is still cached")) {
        return 20;
    }

    std::cout << "scale_atlas: ok\n";
    return 0;
}
