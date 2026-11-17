// SPDX-License-Identifier: MIT

// TextRenderer draws an LCD atlas entry (display-aware-rendering.md section 3.4, slice 2 of #471):
// set_aa_policy maps TextAaMode onto the atlas mode, an LCD entry's quad is one third of its page
// extent wide (the page holds three samples per pixel), and that quad lands in exactly the same
// CSS-space place as the grayscale path at the same scale. The quads are read back out of the
// vertex buffer the renderer bound, so the assertions are about what `draw()` emitted rather than
// about the test's own arithmetic. argv[1] is NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/scale/aa_policy.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"
#include "text_vertex_capture_support.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::rgba8;
using tessera::testsupport::EmittedQuad;
using tessera::testsupport::RecordingDevice;
using tessera::ui::scale::AaConfig;
using tessera::ui::scale::AaFacts;
using tessera::ui::scale::AaPolicy;
using tessera::ui::scale::TextAaMode;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAaMode;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr std::uint16_t kCssSize = 16;
constexpr float kPen = 4.0f;
constexpr float kBaseline = 40.0f;
constexpr float kPageExtent = 256.0f;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_renderer_lcd: " << message << "\n";
    }
    return condition;
}

// The golden policy with the text mode forced, so the only difference between two runs is the AA
// mode (the resolver keeps `Auto` on grayscale until slice 3 flips kAutoAllowsLcd).
AaPolicy policy_with(TextAaMode mode) {
    AaFacts facts;
    AaPolicy policy = tessera::ui::scale::resolve_aa_policy(facts, AaConfig{});
    policy.text_mode = mode;
    return policy;
}

struct FrameStats {
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t texture_binds = 0;
};

FrameStats draw_run(Batched2DRenderer& renderer, RecordingDevice& device,
                    tessera::rendering::rhi::RenderTarget& target, TextRenderer& text,
                    const ShapedRun& run) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    text.draw(renderer, run, kPen, kBaseline, kCssSize, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    FrameStats stats;
    if (auto* recording =
            dynamic_cast<tessera::testsupport::RecordingCommandList*>(commands.get())) {
        const auto* null_stats = recording->stats();
        if (null_stats != nullptr) {
            stats.draws = null_stats->draws;
            stats.vertices = null_stats->vertices;
            stats.texture_binds = null_stats->texture_binds;
        }
    }
    return stats;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_renderer_lcd: missing font path\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_renderer_lcd: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    RecordingDevice device;
    auto target = device.create_render_target({{128, 64}});
    auto white = device.create_texture({{1, 1}});
    auto pipeline = device.create_pipeline({});
    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text(device, rasterizer, {256, 256, 2, 1});
    Batched2DRenderer renderer(device, *pipeline, *white);

    TextStyle style;
    style.font_size = static_cast<float>(kCssSize);
    const ShapedRun run = shaper.shape("Hi", style);
    if (!expect(run.glyphs.size() == 2, "Hi shapes to two glyphs")) {
        return 2;
    }
    bool ok = true;

    // (a) A fresh renderer is the golden path: grayscale, so a caller that never sets a policy
    // draws exactly as before.
    ok &= expect(text.aa_mode() == GlyphAaMode::Grayscale, "a fresh renderer is grayscale");

    // (b) The grayscale reference: the quad is the bitmap's own extent and its UV covers exactly
    // that extent, which is the path that must not move.
    text.begin_frame();
    const FrameStats gray_frame = draw_run(renderer, device, *target, text, run);
    ok &= expect(gray_frame.draws == 1 && gray_frame.vertices == 12,
                 "grayscale: two glyphs batch into one draw");
    const EmittedQuad gray_quad = device.first_quad();
    ok &= expect(gray_quad.valid, "grayscale: the emitted quad is readable from the vertex buffer");
    const GlyphAtlasEntry* gray_entry =
        text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0);
    ok &= expect(gray_entry != nullptr && gray_entry->channels == 1,
                 "grayscale: the entry has one channel");
    const GlyphAtlasEntry gray_copy = gray_entry != nullptr ? *gray_entry : GlyphAtlasEntry{};
    ok &= expect(text.page_texture_count() > 0, "grayscale: a page texture is live");
    ok &= expect(gray_quad.width == static_cast<float>(gray_copy.width),
                 "grayscale: the quad is the bitmap width");
    ok &= expect(gray_quad.uv_width == static_cast<float>(gray_copy.width) / kPageExtent,
                 "grayscale: the UV covers the bitmap's own extent");

    // (c) set_aa_policy maps TextAaMode onto the atlas mode, and the change releases the atlases
    // and page textures (their bitmaps are stale).
    text.set_aa_policy(policy_with(TextAaMode::LcdWhenEligible));
    ok &= expect(text.aa_mode() == GlyphAaMode::Lcd, "LcdWhenEligible selects the LCD atlas");
    ok &= expect(text.page_texture_count() == 0, "an AA-mode change drops every page texture");
    ok &= expect(text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0) ==
                     nullptr,
                 "an AA-mode change releases the atlas entries");

    // (d) The LCD run still draws, and its entry is three samples per pixel wide.
    text.begin_frame();
    const FrameStats lcd_frame = draw_run(renderer, device, *target, text, run);
    ok &= expect(lcd_frame.draws == 1 && lcd_frame.vertices == 12,
                 "LCD: two glyphs still batch into one draw");
    ok &= expect(lcd_frame.texture_binds == 1, "LCD: one atlas page texture");
    const EmittedQuad lcd_quad = device.first_quad();
    ok &= expect(lcd_quad.valid, "LCD: the emitted quad is readable from the vertex buffer");
    const GlyphAtlasEntry* lcd_entry =
        text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0);
    ok &= expect(lcd_entry != nullptr && lcd_entry->channels == 3,
                 "LCD: the entry has three channels");
    if (lcd_entry == nullptr) {
        return 3;
    }
    ok &= expect(lcd_entry->width % 3 == 0, "LCD: the page extent is a whole pixel count");
    ok &= expect(lcd_entry->width / 3 >= gray_copy.width,
                 "LCD: the page extent is three samples per pixel of the footprint");

    // (e) The emitted quad is one third of the page extent wide - the renderer divides the entry
    // width by its channel count - while its UV still covers the whole three-sample extent. A
    // renderer that forgot the division would emit a quad three times too wide.
    ok &= expect(lcd_quad.width == static_cast<float>(lcd_entry->width / 3),
                 "LCD: the emitted quad is the footprint, not the page extent");
    ok &= expect(lcd_quad.width * 3.0f == static_cast<float>(lcd_entry->width),
                 "LCD: the emitted quad is exactly one third of the page extent");
    ok &= expect(lcd_quad.height == static_cast<float>(lcd_entry->height),
                 "LCD: the emitted quad height is the bitmap height");
    ok &= expect(lcd_quad.uv_width == static_cast<float>(lcd_entry->width) / kPageExtent,
                 "LCD: the UV still covers all three samples per pixel");
    ok &= expect(lcd_quad.uv_height == static_cast<float>(lcd_entry->height) / kPageExtent,
                 "LCD: the UV height is the bitmap height");

    // (f) The CSS-space placement is the grayscale path's: same baseline, same pen, same snapping
    // rule. FreeType pads the LCD box by the subpixel geometry, which can move the bearing by one
    // device pixel, so the origin is allowed that much; the top is exact (the padding is
    // horizontal only).
    ok &= expect(lcd_quad.top == gray_quad.top, "LCD: the quad top matches the grayscale path");
    ok &= expect(std::abs(lcd_quad.width - gray_quad.width) <= 1.0f,
                 "LCD: the quad width matches the grayscale path within the subpixel padding");
    ok &= expect(std::abs(lcd_quad.left - gray_quad.left) <= 1.0f,
                 "LCD: the quad origin matches the grayscale path within the subpixel padding");
    const auto expected_left = static_cast<float>(
        std::lround(kPen + run.glyphs[0].x_offset + static_cast<float>(lcd_entry->bearing_x)));
    ok &= expect(lcd_quad.left == expected_left,
                 "LCD: the origin is the whole-device-pixel snap the grayscale path uses");

    // (g) Back to grayscale: the golden path is restored, and the rebuilt entry is the bitmap the
    // first draw produced (the atlas was released, so the placement is recomputed identically).
    text.set_aa_policy(policy_with(TextAaMode::Grayscale));
    ok &=
        expect(text.aa_mode() == GlyphAaMode::Grayscale, "Grayscale restores the grayscale atlas");
    text.begin_frame();
    const FrameStats back = draw_run(renderer, device, *target, text, run);
    ok &= expect(back.draws == 1 && back.vertices == 12 && back.texture_binds == 1,
                 "grayscale: the run batches identically after the round trip");
    const EmittedQuad back_quad = device.first_quad();
    ok &= expect(back_quad.valid && back_quad.width == gray_quad.width &&
                     back_quad.height == gray_quad.height && back_quad.left == gray_quad.left &&
                     back_quad.top == gray_quad.top,
                 "grayscale: the emitted quad is unchanged after the round trip");
    const GlyphAtlasEntry* back_entry =
        text.atlas().find_glyph(run.glyphs[0].id, kCssSize, run.glyphs[0].face, 0);
    ok &= expect(back_entry != nullptr && back_entry->channels == 1,
                 "grayscale: the rebuilt entry has one channel");
    ok &= expect(back_entry != nullptr && back_entry->width == gray_copy.width &&
                     back_entry->height == gray_copy.height &&
                     back_entry->bearing_x == gray_copy.bearing_x &&
                     back_entry->bearing_y == gray_copy.bearing_y,
                 "grayscale: the rebuilt entry is the golden bitmap");

    // (h) The golden policy itself keeps the default: a renderer that applies it never selects LCD.
    text.set_aa_policy(tessera::ui::scale::golden_aa_policy());
    ok &= expect(text.aa_mode() == GlyphAaMode::Grayscale,
                 "the golden policy pins grayscale coverage");

    return ok ? 0 : 1;
}
