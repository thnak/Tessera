// SPDX-License-Identifier: MIT

// Text raster at device size (display-aware-rendering.md section 3.1, T1): the caller's pixel size
// is CSS, the bitmap is rasterized at `round(css * render_scale)` device pixels, the atlas key is
// that device size, and the quad is placed back in CSS units. Also proves the metrics contract -
// the returned advance is the run's CSS advance at every scale - and that the placement math is
// today's whole-pixel snap at 1.0. The null device keeps the draws deterministic; argv[1] is
// NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/scale/aa_policy.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"
#include "text_quad.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::rgba8;
using tessera::ui::scale::AaConfig;
using tessera::ui::scale::AaFacts;
using tessera::ui::scale::AaPolicy;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;
using tessera::ui::text::detail::device_pixel_size;
using tessera::ui::text::detail::GlyphQuad;
using tessera::ui::text::detail::place_glyph_quad;

constexpr std::uint16_t kCssSize = 16;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_device_raster: " << message << "\n";
    }
    return condition;
}

AaPolicy policy_at(float scale) {
    AaFacts facts;
    facts.render_scale = scale;
    return tessera::ui::scale::resolve_aa_policy(facts, AaConfig{});
}

struct FrameStats {
    float advance = 0.0f;
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t texture_binds = 0;
};

FrameStats draw_run(Batched2DRenderer& renderer, tessera::rendering::rhi::Device& device,
                    tessera::rendering::rhi::RenderTarget& target, TextRenderer& text,
                    const ShapedRun& run, float x) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    const float advance = text.draw(renderer, run, x, 40.0f, kCssSize, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    FrameStats stats;
    stats.advance = advance;
    if (auto* null_commands = dynamic_cast<NullCommandList*>(commands.get())) {
        const auto& null_stats = null_commands->stats();
        stats.draws = null_stats.draws;
        stats.vertices = null_stats.vertices;
        stats.texture_binds = null_stats.texture_binds;
    }
    return stats;
}

bool same_entry(const GlyphAtlasEntry& a, const GlyphAtlasEntry& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height &&
           a.bearing_x == b.bearing_x && a.bearing_y == b.bearing_y && a.advance == b.advance &&
           a.page == b.page && a.has_bitmap == b.has_bitmap && a.color == b.color;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_device_raster: missing font path\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_device_raster: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    bool ok = true;

    // (a) The device size is rounded, floored at one pixel and clamped to the rasterizer's range.
    ok = expect(device_pixel_size(kCssSize, 1.0f) == 16, "1.0x keeps the CSS size") && ok;
    ok = expect(device_pixel_size(kCssSize, 1.25f) == 20, "1.25x rasterizes at 20 px") && ok;
    ok = expect(device_pixel_size(kCssSize, 1.5f) == 24, "1.5x rasterizes at 24 px") && ok;
    ok = expect(device_pixel_size(kCssSize, 2.0f) == 32, "2.0x rasterizes at 32 px") && ok;
    ok =
        expect(device_pixel_size(1, 0.25f) == 1, "a sub-pixel result clamps to one device pixel") &&
        ok;
    ok = expect(device_pixel_size(kCssSize, std::numeric_limits<float>::quiet_NaN()) == kCssSize,
                "a NaN scale falls back to 1:1") &&
         ok;
    ok = expect(device_pixel_size(65535, 2.0f) == 65535, "the device size clamps at uint16 max") &&
         ok;

    // (b) At 1.0 the placement is exactly the pre-T1 whole-pixel snap: the same expression the
    // renderer used before the device raster size existed. `left`/`top` are the quad origin, so the
    // bearing and the Y offset are part of the same rounded expression.
    const float pen = 4.3f;
    const float baseline = 40.0f;
    const float x_offset = 0.25f;
    const float y_offset = 2.0f;
    const std::int32_t bearing_x = 1;
    const std::int32_t bearing_y = 12;
    const std::uint32_t bitmap_width = 9;
    const std::uint32_t bitmap_height = 11;
    const GlyphQuad golden_quad =
        place_glyph_quad(pen, baseline, x_offset, y_offset, bearing_x, bearing_y, bitmap_width,
                         bitmap_height, 1.0f, /*subpixel_position=*/false);
    ok = expect(golden_quad.left == std::round(pen + x_offset + static_cast<float>(bearing_x)),
                "1.0: the left edge is the whole-pixel snap") &&
         ok;
    ok = expect(golden_quad.top == std::round(baseline - y_offset - static_cast<float>(bearing_y)),
                "1.0: the top edge is the whole-pixel snap") &&
         ok;
    ok = expect(golden_quad.width == static_cast<float>(bitmap_width) &&
                    golden_quad.height == static_cast<float>(bitmap_height),
                "1.0: the quad is the bitmap's exact size in CSS") &&
         ok;
    const GlyphQuad subpixel_quad =
        place_glyph_quad(pen, baseline, x_offset, y_offset, bearing_x, bearing_y, bitmap_width,
                         bitmap_height, 1.0f, /*subpixel_position=*/true);
    ok = expect(subpixel_quad.left == std::floor(pen + x_offset) + static_cast<float>(bearing_x),
                "1.0: a subpixel-positioned origin still floors exactly as before") &&
         ok;

    // (c) Above 1.0 the origin snaps on the DEVICE grid and the quad divides back into CSS: pen 4.3
    // at 2.0 is device 8.6, whose origin rounds (or floors) to 9/8 device px, and the 9 px bitmap
    // becomes 4.5 CSS px.
    const GlyphQuad scaled = place_glyph_quad(4.3f, 40.0f, 0.0f, 2.0f, 1, 12, 9, 11, 2.0f, false);
    ok = expect(scaled.left == 5.0f, "2.0: the snapped origin converts back to CSS") && ok;
    ok = expect(scaled.top == 32.0f, "2.0: the snapped baseline converts back to CSS") && ok;
    ok = expect(scaled.width == 4.5f && scaled.height == 5.5f,
                "2.0: the bitmap extent converts back to CSS") &&
         ok;
    const GlyphQuad scaled_subpixel =
        place_glyph_quad(4.3f, 40.0f, 0.0f, 2.0f, 1, 12, 9, 11, 2.0f, true);
    ok = expect(scaled_subpixel.left == 4.5f,
                "2.0: a subpixel-positioned origin floors on the device grid") &&
         ok;

    // (d) Renderer: the 1.0 atlas is keyed by the CSS size (which is the device size at 1.0), and
    // the 2.0 atlas is keyed by the doubled device size. The returned advance never moves.
    auto device = tessera::rendering::null::create_null_device({});
    auto target = device->create_render_target({{128, 64}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});
    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text(*device, rasterizer, {256, 256, 2, 1});
    Batched2DRenderer renderer(*device, *pipeline, *white);

    TextStyle style;
    style.font_size = static_cast<float>(kCssSize);
    const ShapedRun run = shaper.shape("Hi", style);
    if (!expect(run.glyphs.size() == 2, "Hi shapes to two glyphs")) {
        return 2;
    }

    text.begin_frame();
    const FrameStats at_one = draw_run(renderer, *device, *target, text, run, 4.0f);
    ok = expect(at_one.draws == 1 && at_one.vertices == 12 && at_one.texture_binds == 1,
                "1.0: two glyphs batch into one draw") &&
         ok;
    ok = expect(at_one.advance == run.advance, "1.0: draw returns the run's CSS advance") && ok;
    const GlyphAtlasEntry* one_entry =
        text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0);
    ok = expect(one_entry != nullptr, "1.0: the atlas is keyed by the CSS device size") && ok;
    const GlyphAtlasEntry entry_at_one = one_entry != nullptr ? *one_entry : GlyphAtlasEntry{};

    text.set_aa_policy(policy_at(2.0f));
    text.begin_frame();
    const FrameStats at_two = draw_run(renderer, *device, *target, text, run, 4.0f);
    ok = expect(at_two.draws == 1 && at_two.vertices == 12 && at_two.texture_binds == 1,
                "2.0: the same run still batches into one draw") &&
         ok;
    ok = expect(at_two.advance == at_one.advance && at_two.advance == run.advance,
                "2.0: metrics are unchanged across scales") &&
         ok;
    const float device_origin = (4.0f + run.glyphs[0].x_offset) * 2.0f;
    const float device_fraction = device_origin - std::floor(device_origin);
    const auto device_offset = static_cast<std::uint8_t>(std::lround(device_fraction * 4.0f) % 4L);
    const GlyphAtlasEntry* two_entry =
        text.atlas().find_glyph(run.glyphs[0].id, 32, run.glyphs[0].face, device_offset);
    ok = expect(two_entry != nullptr, "2.0: the atlas is keyed by the 32 px device size") && ok;
    ok = expect(text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0) ==
                    nullptr,
                "2.0: the CSS pixel size is not an atlas key") &&
         ok;
    ok = expect(two_entry != nullptr && two_entry->width > entry_at_one.width &&
                    two_entry->height > entry_at_one.height,
                "2.0: the bitmap is rasterized bigger, not stretched") &&
         ok;
    ok = expect(two_entry != nullptr && static_cast<float>(two_entry->width) / 2.0f <
                                            static_cast<float>(entry_at_one.width),
                "2.0: the CSS quad is narrower than the device bitmap") &&
         ok;

    // (e) The frame transform the window composes at device scale 2 (ui::scale::content_scale_for):
    // the quad's device-pixel extent is exactly the rasterized bitmap's, so the atlas is neither
    // minified nor magnified, and its origin sits on a whole device pixel. That 1:1 property is
    // what makes T1 sharpen text instead of resampling a bigger bitmap into the old device
    // footprint.
    const tessera::ui::scale::ContentScale frame_transform =
        tessera::ui::scale::content_scale_for(2.0f);
    if (two_entry != nullptr) {
        const GlyphQuad device_quad =
            place_glyph_quad(4.0f, 40.0f, 0.0f, 0.0f, two_entry->bearing_x, two_entry->bearing_y,
                             two_entry->width, two_entry->height, 2.0f, false);
        ok = expect(device_quad.width * frame_transform.x == static_cast<float>(two_entry->width) &&
                        device_quad.height * frame_transform.y ==
                            static_cast<float>(two_entry->height),
                    "device scale 2: the quad covers exactly the bitmap's device pixels") &&
             ok;
        ok = expect(device_quad.left * frame_transform.x ==
                        std::round(4.0f * 2.0f + static_cast<float>(two_entry->bearing_x)),
                    "device scale 2: the snapped origin is a whole device pixel") &&
             ok;
    }

    // (f) Back to the golden policy: the 1.0 step is re-rasterized to the same entry, so every
    // existing caller keeps byte-identical quad inputs.
    text.set_aa_policy(tessera::ui::scale::golden_aa_policy());
    text.begin_frame();
    const FrameStats golden_again = draw_run(renderer, *device, *target, text, run, 4.0f);
    ok =
        expect(golden_again.advance == run.advance, "golden: draw returns the run's CSS advance") &&
        ok;
    const GlyphAtlasEntry* golden_entry =
        text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0);
    ok = expect(golden_entry != nullptr && same_entry(*golden_entry, entry_at_one),
                "golden: the 1.0 bitmap and quad inputs are unchanged") &&
         ok;

    return ok ? 0 : 1;
}
