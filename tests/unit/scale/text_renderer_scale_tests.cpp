// SPDX-License-Identifier: MIT

// TextRenderer selects its glyph atlas by render-scale ladder step (feat/text-scale-atlas).
// Proves: set_render_scale honors quantize_render_scale; a ladder change releases the step being
// left and re-rasterizes into a cold atlas instead of reusing the old page; and drawing at 1.0
// keeps the pre-ladder quad/vertex geometry. The null device makes the draw records
// deterministic; argv[1] is NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::rhi::rgba8;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr std::uint16_t kPixelSize = 16;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_renderer_scale: " << message << "\n";
    }
    return condition;
}

struct FrameStats {
    std::uint32_t draws = 0;
    std::uint32_t vertices = 0;
    std::uint32_t texture_binds = 0;
    std::uint32_t first_vertex = 0;
    bool null_backend = false;
};

FrameStats draw_run(Batched2DRenderer& renderer, tessera::rendering::rhi::Device& device,
                    tessera::rendering::rhi::RenderTarget& target, TextRenderer& text,
                    const ShapedRun& run) {
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    text.draw(renderer, run, 4.0f, 40.0f, kPixelSize, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();

    FrameStats stats;
    auto* null_commands = dynamic_cast<NullCommandList*>(commands.get());
    if (null_commands != nullptr) {
        const auto& null_stats = null_commands->stats();
        stats.null_backend = true;
        stats.draws = null_stats.draws;
        stats.vertices = null_stats.vertices;
        stats.texture_binds = null_stats.texture_binds;
        if (!null_stats.draw_records.empty()) {
            stats.first_vertex = null_stats.draw_records.front().first_vertex;
        }
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
        std::cerr << "text_renderer_scale: missing font path\n";
        return 1;
    }

    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_renderer_scale: cannot load fixture: " << loaded.error().message << "\n";
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
    const ShapedRun run = shaper.shape("Hi", style);
    if (!expect(run.glyphs.size() == 2, "Hi shapes to two glyphs")) {
        return 2;
    }

    // (a) set_render_scale stores the quantized ladder step, not the requested value.
    struct Row {
        float input;
        float expected;
        const char* message;
    };
    const Row rows[] = {
        {1.0f, 1.0f, "1.0 stays 1.0"},
        {1.0833f, 1.0f, "1.0833 (104 dpi) rounds to 1.0"},
        {1.1667f, 1.25f, "1.1667 (112 dpi) rounds to 1.25"},
        {1.25f, 1.25f, "1.25 stays 1.25"},
        {1.375f, 1.5f, "the 1.375 tie rounds up to 1.5"},
        {1.5f, 1.5f, "1.5 stays 1.5"},
        {1.6f, 1.5f, "1.6 rounds down to 1.5"},
        {1.75f, 2.0f, "the 1.75 tie rounds up to 2.0"},
        {2.0f, 2.0f, "2.0 stays 2.0"},
        {3.0f, 2.0f, "3.0 clamps to 2.0"},
        {0.5f, 1.0f, "0.5 clamps to 1.0"},
        {0.0f, 1.0f, "0.0 clamps to 1.0"},
        {-2.0f, 1.0f, "a negative scale clamps to 1.0"},
    };
    if (!expect(text.render_scale() == 1.0f, "a fresh renderer draws at the ladder floor")) {
        return 3;
    }
    for (const Row& row : rows) {
        text.set_render_scale(row.input);
        if (!expect(text.render_scale() == row.expected, row.message)) {
            return 3;
        }
    }
    text.set_render_scale(std::numeric_limits<float>::quiet_NaN());
    if (!expect(text.render_scale() == 1.0f, "NaN quantizes to the ladder floor")) {
        return 3;
    }
    text.set_render_scale(1.0f);

    // (c) Drawing at 1.0 must keep the exact pre-ladder draw records and quad inputs. The
    // reference is a plain single-step GlyphAtlas: TextRenderer's 1.0 atlas has to pack the same
    // rect, bearings, advance, page, and flags for every glyph.
    text.begin_frame();
    const FrameStats first = draw_run(renderer, *device, *target, text, run);
    if (!expect(first.null_backend, "the null backend recorded the draw") ||
        !expect(first.draws == 1, "1.0: two glyphs batch into one draw") ||
        !expect(first.vertices == 12, "1.0: two glyphs emit twelve vertices") ||
        !expect(first.texture_binds == 1, "1.0: one atlas page texture") ||
        !expect(first.first_vertex == 0, "1.0: the run starts at vertex 0") ||
        !expect(renderer.draw_calls() == 1 && renderer.vertex_count() == 12,
                "1.0: the renderer counters match the draw records")) {
        return 4;
    }
    if (!expect(text.atlas().stats().misses == 2 && text.atlas().stats().hits == 0,
                "1.0: the first draw rasterizes both glyphs") ||
        !expect(text.atlas().page_count() == 1, "1.0: both glyphs fit one page") ||
        !expect(text.page_texture_count() == 1, "1.0: one page texture is live")) {
        return 5;
    }

    GlyphAtlas reference(rasterizer, {64, 64, 1, 1});
    std::vector<GlyphAtlasEntry> expected_1x;
    expected_1x.reserve(run.glyphs.size());
    for (const auto& glyph : run.glyphs) {
        expected_1x.push_back(reference.get_glyph(glyph.id, kPixelSize, glyph.face));
    }
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
        const GlyphAtlasEntry* actual =
            text.atlas().find_glyph(run.glyphs[i].id, kPixelSize, run.glyphs[i].face);
        if (!expect(actual != nullptr, "1.0: the drawn glyph is in the active atlas") ||
            !expect(actual != nullptr && same_entry(*actual, expected_1x[i]),
                    "1.0: the quad geometry matches the pre-ladder atlas")) {
            return 6;
        }
    }

    // A same-step set is a no-op: nothing is released and the cached glyphs keep hitting.
    text.set_render_scale(1.0833f);
    if (!expect(text.render_scale() == 1.0f, "1.0833 keeps the 1.0 step") ||
        !expect(text.page_texture_count() == 1, "a same-step set keeps the page textures") ||
        !expect(text.atlas().stats().misses == 2 && text.atlas().stats().hits == 0,
                "a same-step set does not rebuild the atlas")) {
        return 7;
    }
    text.begin_frame();
    const FrameStats repeat = draw_run(renderer, *device, *target, text, run);
    if (!expect(repeat.draws == 1 && repeat.vertices == 12 && repeat.texture_binds == 1,
                "1.0: the second frame batches identically") ||
        !expect(text.atlas().stats().hits == 2, "1.0: the second frame hits the atlas")) {
        return 7;
    }

    // (b) A ladder change releases the step being left and rebuilds glyphs on the new step.
    text.set_render_scale(2.0f);
    if (!expect(text.render_scale() == 2.0f, "2.0 selects the 2.0 step") ||
        !expect(text.page_texture_count() == 0,
                "a ladder change releases the previous step's page textures") ||
        !expect(text.atlas().stats().misses == 0 && text.atlas().stats().hits == 0,
                "the new step starts with a cold atlas")) {
        return 8;
    }
    text.begin_frame();
    const FrameStats high = draw_run(renderer, *device, *target, text, run);
    if (!expect(high.draws == 1 && high.vertices == 12 && high.texture_binds == 1,
                "2.0: the same run batches identically") ||
        !expect(text.atlas().stats().misses == 2 && text.atlas().stats().hits == 0,
                "2.0: glyphs are re-rasterized into the new step, not reused from the 1.0 page") ||
        !expect(text.page_texture_count() == 1, "2.0: the new step owns its page texture")) {
        return 9;
    }

    // A round trip back to 1.0 is a genuine step change too: the 1.0 atlas was released when the
    // renderer left it, so 1.0 starts cold and is rebuilt rather than serving its old page.
    text.set_render_scale(1.0f);
    if (!expect(text.render_scale() == 1.0f, "returning to 1.0 selects the 1.0 step") ||
        !expect(text.page_texture_count() == 0,
                "returning to 1.0 releases the 2.0 page textures") ||
        !expect(text.atlas().stats().misses == 0 && text.atlas().stats().hits == 0,
                "returning to 1.0 starts cold")) {
        return 10;
    }
    text.begin_frame();
    const FrameStats back = draw_run(renderer, *device, *target, text, run);
    if (!expect(back.draws == 1 && back.vertices == 12 && back.texture_binds == 1,
                "1.0: the rebuilt step batches identically") ||
        !expect(text.atlas().stats().misses == 2 && text.atlas().stats().hits == 0,
                "1.0 is re-rasterized after the ladder round trip")) {
        return 11;
    }

    std::cout << "text_renderer_scale: ok\n";
    return 0;
}
