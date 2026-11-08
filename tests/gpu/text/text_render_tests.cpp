// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::rgba8;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_render: " << message << "\n";
    }
    return condition;
}

struct FrameStats {
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t texture_binds = 0;
};

FrameStats frame(Batched2DRenderer& renderer, tessera::rendering::rhi::Device& device,
                 tessera::rendering::rhi::RenderTarget& target, TextRenderer& text,
                 const tessera::ui::text::ShapedRun& run, std::uint16_t pixel_size) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    text.draw(renderer, run, 4.0f, 40.0f, pixel_size, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    FrameStats stats;
    auto* null_commands = dynamic_cast<NullCommandList*>(commands.get());
    if (null_commands != nullptr) {
        stats.draws = null_commands->stats().draws;
        stats.vertices = null_commands->stats().vertices;
        stats.texture_binds = null_commands->stats().texture_binds;
    }
    return stats;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_render: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_render: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    auto device = tessera::rendering::null::create_null_device({});
    auto target = device->create_render_target({{128, 64}});
    auto white = device->create_texture({{1, 1}});
    auto pipeline = device->create_pipeline({});

    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text(*device, rasterizer, {64, 64, 1, 1});
    Batched2DRenderer renderer(*device, *pipeline, *white);

    TextStyle style;
    style.font_size = 16.0f;
    const tessera::ui::text::ShapedRun run = shaper.shape("Hi", style);
    if (!expect(run.glyphs.size() == 2, "Hi shapes to two glyphs")) {
        return 2;
    }

    text.begin_frame();
    const FrameStats first = frame(renderer, *device, *target, text, run, 16);
    if (!expect(first.draws == 1, "both glyphs batch into one draw") ||
        !expect(first.vertices == 12, "two glyphs emit twelve vertices") ||
        !expect(first.texture_binds == 1, "one atlas page texture")) {
        return 3;
    }

    const std::uint64_t hits = text.atlas().stats().hits;
    text.begin_frame();
    const FrameStats second = frame(renderer, *device, *target, text, run, 16);
    if (!expect(text.atlas().stats().hits > hits, "second frame hits the atlas") ||
        !expect(second.draws == 1 && second.vertices == 12, "second frame batches identically")) {
        return 4;
    }

    const tessera::ui::text::ShapedRun phrased = shaper.shape("A B", style);
    text.begin_frame();
    const FrameStats spaced = frame(renderer, *device, *target, text, phrased, 16);
    if (!expect(spaced.vertices == 12, "space emits no quad") ||
        !expect(spaced.draws == 1, "two visible glyphs batch together")) {
        return 5;
    }

    std::cout << "text_render: ok\n";
    return 0;
}
