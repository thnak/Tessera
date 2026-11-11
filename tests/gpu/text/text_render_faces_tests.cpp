// SPDX-License-Identifier: MIT
//
// A font stack through TextRenderer on the null device (issue #12 slice b): glyphs of two faces
// are rasterized by their own rasterizers, share one atlas page and batch into one draw.
// argv[1] is NotoSans, argv[2] is NotoSansHebrew.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/text/fallback_shaper.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <iostream>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::rgba8;
using tessera::ui::text::FallbackShaper;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_render_faces: " << message << "\n";
    }
    return condition;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "text_render_faces: need the NotoSans and NotoSansHebrew paths\n";
        return 64;
    }
    const auto sans = FontFace::load_from_file(argv[1]);
    const auto hebrew = FontFace::load_from_file(argv[2]);
    if (!sans || !hebrew) {
        std::cerr << "text_render_faces: cannot load the fonts\n";
        return 65;
    }

    auto device = tessera::rendering::null::create_null_device({});
    auto target = device->create_render_target({{128, 64}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});

    GlyphRasterizer sans_raster(*sans);
    GlyphRasterizer hebrew_raster(*hebrew);
    TextRenderer text(*device, {&sans_raster, &hebrew_raster}, {64, 64, 1, 1});
    FallbackShaper shaper({*sans, *hebrew});
    Batched2DRenderer renderer(*device, *pipeline, *white);

    TextStyle style;
    style.font_size = 16.0f;
    // Two Latin letters and two Hebrew letters: four visible glyphs from two faces.
    const auto run = shaper.shape("ab\xD7\xA9\xD7\x9C", style);
    if (!expect(run.glyphs.size() == 4 && run.missing_glyphs == 0, "four glyphs")) {
        return 2;
    }

    auto commands = device->create_command_list();
    text.begin_frame();
    renderer.begin(*commands, *target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    const float advance = text.draw(renderer, run, 4.0f, 40.0f, 16, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    auto* null_commands = dynamic_cast<NullCommandList*>(commands.get());
    if (!expect(null_commands != nullptr, "null command list") ||
        !expect(null_commands->stats().vertices == 24, "four glyphs emit 24 vertices") ||
        !expect(null_commands->stats().draws == 1, "both faces batch into one draw") ||
        !expect(null_commands->stats().texture_binds == 1, "one shared atlas page") ||
        !expect(text.atlas().stats().misses == 4 && text.atlas().page_count() == 1,
                "four glyphs rasterized into one page") ||
        !expect(advance > 0.0f && advance == run.advance, "the advance spans both faces")) {
        return 3;
    }

    std::cout << "text_render_faces: ok\n";
    return 0;
}
