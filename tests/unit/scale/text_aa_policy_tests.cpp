// SPDX-License-Identifier: MIT

// Text consumes the AA policy (display-aware-rendering.md S5 hinting, S6 subpixel positioning and
// T1 device raster size): the rasterizer honours the hinting mode and a quarter-pixel origin
// offset, the atlas keys glyphs by that offset and by the device pixel size the policy's render
// scale asks for, a hinting change releases every atlas, and TextRenderer::set_aa_policy applies
// the policy's hinting and subpixel flag. The default stays the golden policy. The null device
// keeps draw records deterministic; argv[1] is NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/scale/aa_policy.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::rgba8;
using tessera::ui::scale::AaConfig;
using tessera::ui::scale::AaFacts;
using tessera::ui::scale::AaPolicy;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphBitmap;
using tessera::ui::text::GlyphHinting;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ScaleGlyphAtlas;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr std::uint16_t kPixelSize = 16;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_aa_policy: " << message << "\n";
    }
    return condition;
}

bool same_bitmap(const GlyphBitmap& a, const GlyphBitmap& b) {
    return a.width == b.width && a.height == b.height && a.bearing_x == b.bearing_x &&
           a.bearing_y == b.bearing_y && a.coverage == b.coverage;
}

AaPolicy policy_at(float scale) {
    AaFacts facts;
    facts.render_scale = scale;
    return tessera::ui::scale::resolve_aa_policy(facts, AaConfig{});
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_aa_policy: missing font path\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_aa_policy: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    bool ok = true;

    // Rasterizer: every hinting mode renders, and each is repeatable.
    const std::uint32_t h_glyph = rasterizer.glyph_index(U'H');
    for (const GlyphHinting hinting :
         {GlyphHinting::Font, GlyphHinting::Light, GlyphHinting::None}) {
        const GlyphBitmap first = rasterizer.rasterize_glyph(h_glyph, kPixelSize, hinting);
        const GlyphBitmap second = rasterizer.rasterize_glyph(h_glyph, kPixelSize, hinting);
        ok &= expect(first.width > 0 && first.height > 0, "hinted glyph has coverage");
        ok &= expect(same_bitmap(first, second), "a hinting mode is deterministic");
    }
    ok &=
        expect(same_bitmap(rasterizer.rasterize_glyph(h_glyph, kPixelSize),
                           rasterizer.rasterize_glyph(h_glyph, kPixelSize, GlyphHinting::Font, 0)),
               "the defaults are the font-hinted whole-pixel glyph");

    // A half-pixel origin offset changes the coverage columns; the same offset repeats.
    const GlyphBitmap whole =
        rasterizer.rasterize_glyph(h_glyph, kPixelSize, GlyphHinting::None, 0);
    const GlyphBitmap half = rasterizer.rasterize_glyph(h_glyph, kPixelSize, GlyphHinting::None, 2);
    ok &= expect(!same_bitmap(whole, half), "a half-pixel offset changes the bitmap");
    ok &= expect(
        same_bitmap(half, rasterizer.rasterize_glyph(h_glyph, kPixelSize, GlyphHinting::None, 2)),
        "a subpixel offset is deterministic");

    // Atlas: the offset is part of the key, and an out-of-range offset clamps to the last step.
    ScaleGlyphAtlas scale_atlas(rasterizer, {64, 64, 1, 1});
    auto& atlas = scale_atlas.atlas_for(1.0f);
    const auto whole_entry = atlas.get_glyph(h_glyph, kPixelSize, 0, 0);
    const auto half_entry = atlas.get_glyph(h_glyph, kPixelSize, 0, 2);
    ok &= expect(whole_entry.has_bitmap && half_entry.has_bitmap, "both offsets are placed");
    ok &= expect(whole_entry.x != half_entry.x || whole_entry.y != half_entry.y,
                 "two offsets are two atlas entries");
    const auto misses = atlas.stats().misses;
    (void)atlas.get_glyph(h_glyph, kPixelSize, 0, 2);
    ok &= expect(atlas.stats().misses == misses, "the same offset is a hit");
    const auto last = atlas.get_glyph(h_glyph, kPixelSize, 0, 3);
    const auto clamped = atlas.get_glyph(h_glyph, kPixelSize, 0, 9);
    ok &= expect(last.x == clamped.x && last.y == clamped.y, "an offset above 3 clamps to 3");
    ok &= expect(atlas.find_glyph(h_glyph, kPixelSize, 0, 1) == nullptr,
                 "an unrequested offset is absent");

    // Hinting change releases every atlas; the same value is a no-op.
    ok &= expect(scale_atlas.atlas_count() == 1, "one atlas is live");
    scale_atlas.set_hinting(GlyphHinting::Font);
    ok &= expect(scale_atlas.atlas_count() == 1, "the value already in force is a no-op");
    scale_atlas.set_hinting(GlyphHinting::None);
    ok &= expect(scale_atlas.atlas_count() == 0, "a hinting change releases the atlases");
    ok &= expect(scale_atlas.hinting() == GlyphHinting::None, "the new hinting is stored");

    // TextRenderer: the default is the golden policy; set_aa_policy applies hinting and subpixel.
    auto device = tessera::rendering::null::create_null_device({});
    auto target = device->create_render_target({{128, 64}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    TextRenderer text(*device, rasterizer, {64, 64, 1, 1});
    Batched2DRenderer renderer(*device, *pipeline, *white);
    ok &= expect(text.hinting() == GlyphHinting::Font && !text.subpixel_position(),
                 "a fresh renderer has the golden policy");

    TextStyle style;
    style.font_size = 16.0f;
    const ShapedRun run = shaper.shape("Hi", style);
    ok &= expect(run.glyphs.size() == 2, "Hi shapes to two glyphs");

    const auto draw_at = [&](float x) {
        auto commands = device->create_command_list();
        renderer.begin(*commands, *target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
        text.draw(renderer, run, x, 40.0f, kPixelSize, rgba8(255, 255, 255));
        renderer.end();
        commands->submit();
    };

    text.set_aa_policy(policy_at(1.25f));
    ok &= expect(text.hinting() == GlyphHinting::Light, "scale 1.25 selects light hinting");
    ok &= expect(text.subpixel_position(), "scale 1.25 turns subpixel positioning on");
    ok &= expect(text.render_scale() == 1.25f, "the policy's render scale is selected");

    draw_at(4.3f);
    const std::uint32_t pages = text.page_texture_count();
    ok &= expect(pages > 0, "a draw uploads a page");
    // The glyph is rasterized at css_px * 1.25 device pixels, and its origin fraction is taken on
    // that device grid: a quarter-device-pixel offset is cached, whole is not.
    ok &= expect(text.render_scale() == 1.25f, "the policy's 1.25 step is active");
    const std::uint16_t device_size =
        static_cast<std::uint16_t>(std::lround(static_cast<float>(kPixelSize) * 1.25f));
    const float origin = (4.3f + run.glyphs[0].x_offset) * 1.25f;
    const float fraction = origin - std::floor(origin);
    const std::uint8_t expected_offset =
        static_cast<std::uint8_t>(std::lround(fraction * 4.0f)) % 4;
    ok &= expect(text.atlas().find_glyph(run.glyphs[0].id, device_size, run.glyphs[0].face,
                                         expected_offset) != nullptr,
                 "the glyph was rasterized at its quarter-device-pixel offset");
    ok &= expect(text.atlas().find_glyph(run.glyphs[0].id, kPixelSize, run.glyphs[0].face,
                                         expected_offset) == nullptr,
                 "the atlas is keyed by the device pixel size, not the CSS size");

    text.set_aa_policy(policy_at(1.25f));
    ok &= expect(text.page_texture_count() == pages, "re-applying the same policy keeps the pages");

    text.set_aa_policy(policy_at(2.0f));
    ok &= expect(text.hinting() == GlyphHinting::None, "scale 2.0 turns hinting off");
    ok &= expect(text.page_texture_count() == 0, "a hinting change drops every page texture");
    draw_at(4.3f);
    ok &= expect(text.page_texture_count() > 0, "drawing again rebuilds the pages on demand");

    text.set_aa_policy(tessera::ui::scale::golden_aa_policy());
    ok &= expect(text.hinting() == GlyphHinting::Font && !text.subpixel_position() &&
                     text.render_scale() == 1.0f,
                 "the golden policy restores font hints, whole pixels and scale 1.0");

    return ok ? 0 : 1;
}
