// SPDX-License-Identifier: MIT

// TextRenderer uploads only the rectangle an atlas page changed (issue #681): a glyph that adds a
// few texels to a page moves those texels, not the page. The RecordingDevice counts every texture
// write and keeps the bytes its "GPU" copy holds, so the test proves both halves: the byte count is
// the glyph's rectangle, and the GPU copy still equals the atlas's CPU page afterwards (a
// rectangle that missed a texel would leave a stale one). argv[1] is NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"
#include "text_vertex_capture_support.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::rgba8;
using tessera::testsupport::CountingTexture;
using tessera::testsupport::RecordingDevice;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

constexpr std::uint16_t kCssSize = 16;
constexpr std::uint32_t kPage = 256;
constexpr std::size_t kPageBytes = static_cast<std::size_t>(kPage) * kPage * 4;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_renderer_dirty_upload: " << message << "\n";
    }
    return condition;
}

void draw(Batched2DRenderer& renderer, RecordingDevice& device,
          tessera::rendering::rhi::RenderTarget& target, TextRenderer& text, const ShapedRun& run) {
    text.begin_frame();
    auto commands = device.create_command_list();
    renderer.begin(*commands, target, tessera::rendering::rhi::RenderTargetDesc{{128, 64}});
    text.draw(renderer, run, 4.0f, 40.0f, kCssSize, rgba8(255, 255, 255));
    renderer.end();
    commands->submit();
}

// The page texture the renderer made: the only live texture of the page's extent.
const CountingTexture* page_texture(const RecordingDevice& device) {
    for (const CountingTexture* texture : device.live_textures()) {
        if (texture->extent().width == kPage && texture->extent().height == kPage) {
            return texture;
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_renderer_dirty_upload: missing font path\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_renderer_dirty_upload: cannot load fixture: " << loaded.error().message
                  << "\n";
        return 1;
    }

    RecordingDevice device;
    auto target = device.create_render_target({{128, 64}});
    auto white = device.create_texture({{1, 1}});
    auto pipeline = device.create_pipeline({});
    GlyphRasterizer rasterizer(*loaded);
    HarfBuzzShaper shaper(*loaded);
    TextRenderer text(device, rasterizer, {kPage, kPage, 2, 1});
    Batched2DRenderer renderer(device, *pipeline, *white);

    TextStyle style;
    style.font_size = static_cast<float>(kCssSize);
    const ShapedRun first_run = shaper.shape("H", style);
    const ShapedRun second_run = shaper.shape("Hi", style);
    if (!expect(first_run.glyphs.size() == 1 && second_run.glyphs.size() == 2,
                "the fixture shapes H and Hi to one and two glyphs")) {
        return 2;
    }
    bool ok = true;

    // Frame 1 creates the page texture from the whole page: creation is not a write.
    draw(renderer, device, *target, text, first_run);
    const CountingTexture* page = page_texture(device);
    if (!expect(page != nullptr, "the renderer made a page texture")) {
        return 3;
    }
    // Issue #949: the new page uploads its used extent (one region write), not the whole page.
    const GlyphAtlasEntry* first_glyph =
        text.atlas().find_glyph(first_run.glyphs[0].id, kCssSize, first_run.glyphs[0].face, 0);
    ok &= expect(first_glyph != nullptr && first_glyph->has_bitmap,
                 "the first glyph is in the atlas");
    ok &= expect(device.texture_uploads().whole_writes == 0 &&
                     device.texture_uploads().region_writes == 1,
                 "a new page texture is one region write of its used extent");
    ok &= expect(device.texture_uploads().region_bytes * 4 < kPageBytes,
                 "the first upload is a fraction of the page");
    ok &= expect(page->gpu_bytes().size() == kPageBytes &&
                     std::equal(page->gpu_bytes().begin(),
                                page->gpu_bytes().begin() +
                                    kPage * 4 * (first_glyph->y + first_glyph->height + 1),
                                text.atlas().page_pixels(first_glyph->page).begin()),
                 "the used rows of the GPU copy equal the atlas page");
    const std::size_t first_region_writes = device.texture_uploads().region_writes;
    const std::size_t first_region_bytes = device.texture_uploads().region_bytes;

    // Frame 2 adds one glyph: one region write of exactly that glyph's rectangle, no whole write.
    draw(renderer, device, *target, text, second_run);
    const GlyphAtlasEntry* added =
        text.atlas().find_glyph(second_run.glyphs[1].id, kCssSize, second_run.glyphs[1].face, 0);
    if (!expect(added != nullptr && added->has_bitmap, "the second glyph is in the atlas")) {
        return 4;
    }
    const auto& stats = device.texture_uploads();
    ok &= expect(stats.whole_writes == 0, "adding a glyph does not rewrite the whole page");
    ok &= expect(stats.region_writes == first_region_writes + 1,
                 "adding a glyph is one region write");
    ok &= expect(stats.region_bytes - first_region_bytes ==
                     static_cast<std::size_t>(added->width) * added->height * 4,
                 "the region write moves exactly the new glyph's rectangle");
    ok &= expect((stats.region_bytes - first_region_bytes) * 8 < kPageBytes,
                 "the region write is a small fraction of the page");
    // Rows below the used extent were never uploaded (issue #949), so compare the synced rows.
    ok &= expect(
        page->gpu_bytes().size() == kPageBytes &&
            std::equal(page->gpu_bytes().begin(),
                       page->gpu_bytes().begin() + kPage * 4 * (added->y + added->height + 1),
                       text.atlas().page_pixels(added->page).begin()),
        "the synced rows of the GPU copy equal the atlas page after the region write");

    // Frame 3 draws the same text: every glyph is cached, so nothing is uploaded.
    const std::size_t writes_before = stats.region_writes + stats.whole_writes;
    draw(renderer, device, *target, text, second_run);
    ok &= expect(stats.region_writes + stats.whole_writes == writes_before,
                 "a frame with only cached glyphs uploads nothing");

    if (!ok) {
        std::cerr << "text_renderer_dirty_upload: FAILED\n";
        return 5;
    }
    std::cout << "text_renderer_dirty_upload: ok\n";
    return 0;
}
