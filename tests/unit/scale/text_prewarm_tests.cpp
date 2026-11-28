// SPDX-License-Identifier: MIT

// Off-thread glyph prewarm for a render-scale step (issue #948). The deterministic half drives a
// ManualIoExecutor by hand, so the test decides exactly when the worker body and the UI-thread
// completion run and can assert what the UI thread has and has not observed in between. One case
// runs a real WorkerIoExecutor thread to cover the cross-thread hand-off. argv[1] is
// NotoSans-Regular.ttf.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/manual_io_executor.hpp"
#include "tessera/runtime/worker_io_executor.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/glyph_prewarm.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "text_vertex_capture_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

namespace {

using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::rhi::rgba8;
using tessera::runtime::CompletionQueue;
using tessera::runtime::ManualIoExecutor;
using tessera::runtime::WorkerIoExecutor;
using tessera::testsupport::RecordingDevice;
using tessera::ui::text::FontFace;
using tessera::ui::text::GlyphAaMode;
using tessera::ui::text::GlyphAtlas;
using tessera::ui::text::GlyphAtlasConfig;
using tessera::ui::text::GlyphAtlasEntry;
using tessera::ui::text::GlyphAtlasKey;
using tessera::ui::text::GlyphHinting;
using tessera::ui::text::GlyphPrewarm;
using tessera::ui::text::GlyphRasterizer;
using tessera::ui::text::HarfBuzzShaper;
using tessera::ui::text::ShapedRun;
using tessera::ui::text::TextRenderer;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_prewarm: " << message << "\n";
    }
    return condition;
}

std::vector<GlyphAtlasKey> ascii_keys(GlyphRasterizer& rasterizer, std::uint16_t size) {
    std::vector<GlyphAtlasKey> keys;
    for (char32_t c = U'!'; c <= U'~'; ++c) {
        keys.push_back({rasterizer.glyph_index(c), size, 0, 0});
    }
    std::sort(keys.begin(), keys.end(), [](const GlyphAtlasKey& l, const GlyphAtlasKey& r) {
        return l.glyph_index < r.glyph_index;
    });
    return keys;
}

GlyphAtlasConfig small_pages() {
    GlyphAtlasConfig config;
    config.page_width = 256;
    config.page_height = 256;
    config.max_pages = 4;
    return config;
}

bool start_job(GlyphPrewarm& prewarm, tessera::runtime::IoTaskExecutor& executor,
               GlyphRasterizer& rasterizer, const std::vector<GlyphAtlasKey>& keys) {
    GlyphRasterizer* stack[] = {&rasterizer};
    return prewarm.start(executor, stack, keys, GlyphHinting::Font, GlyphAaMode::Grayscale);
}

GlyphAtlas lazy_reference(GlyphRasterizer& rasterizer, const std::vector<GlyphAtlasKey>& keys) {
    GlyphAtlas reference(rasterizer, small_pages());
    for (const GlyphAtlasKey& key : keys) {
        static_cast<void>(reference.get_glyph(key.glyph_index, key.pixel_size, key.face, 0));
    }
    return reference;
}

bool pages_equal(const GlyphAtlas& lhs, const GlyphAtlas& rhs) {
    if (lhs.page_count() != rhs.page_count()) {
        return false;
    }
    for (std::uint16_t page = 0; page < lhs.page_count(); ++page) {
        const auto a = lhs.page_pixels(page);
        const auto b = rhs.page_pixels(page);
        if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin())) {
            return false;
        }
    }
    return true;
}

// The same glyph in two atlases: identical metrics and identical page texels in its rectangle.
bool same_glyph(const GlyphAtlas& lhs, const GlyphAtlasEntry& a, const GlyphAtlas& rhs,
                const GlyphAtlasEntry& b) {
    if (a.width != b.width || a.height != b.height || a.bearing_x != b.bearing_x ||
        a.bearing_y != b.bearing_y || a.advance != b.advance || a.has_bitmap != b.has_bitmap) {
        return false;
    }
    if (!a.has_bitmap) {
        return true;
    }
    const auto pa = lhs.page_pixels(a.page);
    const auto pb = rhs.page_pixels(b.page);
    for (std::uint32_t row = 0; row < a.height; ++row) {
        const std::size_t oa = (static_cast<std::size_t>(a.y + row) * lhs.page_width() + a.x) * 4;
        const std::size_t ob = (static_cast<std::size_t>(b.y + row) * rhs.page_width() + b.x) * 4;
        if (!std::equal(pa.begin() + oa, pa.begin() + oa + a.width * 4, pb.begin() + ob)) {
            return false;
        }
    }
    return true;
}

bool atlas_level(GlyphRasterizer& rasterizer) {
    bool ok = true;
    const std::vector<GlyphAtlasKey> keys = ascii_keys(rasterizer, 20);
    const GlyphAtlas reference = lazy_reference(rasterizer, keys);

    ManualIoExecutor executor;
    GlyphPrewarm prewarm;
    GlyphAtlas target(rasterizer, small_pages());
    ok &= expect(start_job(prewarm, executor, rasterizer, keys), "the executor accepts the job");
    ok &= expect(prewarm.pending() && !prewarm.ready(), "a started job is pending, not ready");
    ok &= expect(executor.stats().posted == 1 && executor.stats().executed == 0,
                 "start() only enqueues: no glyph is rasterized on the calling thread");
    ok &= expect(prewarm.install(target) == 0 && target.page_count() == 0,
                 "installing before the worker ran changes nothing");

    ok &= expect(executor.run_one(), "the worker body runs");
    ok &= expect(!prewarm.ready() && prewarm.install(target) == 0,
                 "the job is not ready until its completion has run on the UI side");
    ok &= expect(executor.run_completion_one(), "the completion runs");
    ok &= expect(prewarm.ready(), "the job is ready after its completion");

    ok &= expect(prewarm.install(target) == keys.size(), "every glyph is cached by the install");
    ok &= expect(!prewarm.pending(), "installing ends the job");
    ok &= expect(target.stats().misses == 0, "a prewarmed glyph is not a rasterization miss");
    ok &= expect(pages_equal(reference, target),
                 "the prewarmed pages equal the pages the UI thread would have rasterized");
    const GlyphAtlasEntry& hit = target.get_glyph(keys[3].glyph_index, 20);
    ok &= expect(target.stats().hits == 1 && target.stats().misses == 0 && hit.has_bitmap,
                 "drawing a prewarmed glyph is a hit");

    // A newer start abandons the older job: its glyphs are never installed.
    GlyphPrewarm supersede;
    GlyphAtlas fresh(rasterizer, small_pages());
    const std::vector<GlyphAtlasKey> first_keys = ascii_keys(rasterizer, 12);
    ok &= expect(start_job(supersede, executor, rasterizer, first_keys), "first job accepted");
    ok &= expect(start_job(supersede, executor, rasterizer, keys), "second job accepted");
    static_cast<void>(executor.run(8));
    static_cast<void>(executor.run_completions(8));
    ok &= expect(supersede.ready(), "the newer job is ready");
    static_cast<void>(supersede.install(fresh));
    ok &= expect(fresh.find_glyph(keys[0].glyph_index, 12) == nullptr &&
                     fresh.find_glyph(keys[0].glyph_index, 20) != nullptr,
                 "only the newer job's glyphs are installed");

    // cancel() drops the job even when its worker already ran.
    GlyphPrewarm cancelled;
    static_cast<void>(start_job(cancelled, executor, rasterizer, keys));
    static_cast<void>(executor.run(8));
    cancelled.cancel();
    static_cast<void>(executor.run_completions(8));
    GlyphAtlas untouched(rasterizer, small_pages());
    ok &= expect(!cancelled.pending() && !cancelled.ready() && cancelled.install(untouched) == 0 &&
                     untouched.page_count() == 0,
                 "a cancelled job installs nothing");

    // A full executor refuses the job and leaves nothing pending.
    ManualIoExecutor tiny(1);
    GlyphPrewarm busy;
    GlyphPrewarm refused;
    static_cast<void>(start_job(busy, tiny, rasterizer, keys));
    ok &= expect(!start_job(refused, tiny, rasterizer, keys) && !refused.pending(),
                 "a refused job is not pending");
    static_cast<void>(tiny.clear());
    return ok;
}

bool real_thread(GlyphRasterizer& rasterizer) {
    const std::vector<GlyphAtlasKey> keys = ascii_keys(rasterizer, 24);
    const GlyphAtlas reference = lazy_reference(rasterizer, keys);

    CompletionQueue queue(64);
    GlyphAtlas target(rasterizer, small_pages());
    GlyphPrewarm prewarm;
    {
        WorkerIoExecutor executor(queue);
        if (!expect(start_job(prewarm, executor, rasterizer, keys),
                    "the worker executor accepts the job")) {
            return false;
        }
        // The UI thread keeps rasterizing from its own face while the worker runs.
        GlyphAtlas busy(rasterizer, small_pages());
        static_cast<void>(busy.get_glyph(keys[0].glyph_index, 33));
        // Drain completions the way the loop does; the bound only guards against a hung worker.
        for (std::size_t spin = 0; spin < 50'000'000 && !prewarm.ready(); ++spin) {
            static_cast<void>(queue.run(8));
            std::this_thread::yield();
        }
        executor.shutdown();
    }
    bool ok = expect(prewarm.ready(), "the worker finished and its completion ran");
    ok &= expect(prewarm.install(target) == keys.size() && pages_equal(reference, target),
                 "worker output equals UI-thread output byte for byte");
    return ok;
}

bool renderer_level(const FontFace& face) {
    bool ok = true;
    RecordingDevice device;
    auto target = device.create_render_target({{256, 64}});
    auto white = device.create_texture({{1, 1}});
    auto pipeline = device.create_pipeline({});
    GlyphRasterizer rasterizer(face);
    GlyphRasterizer sync_rasterizer(face);
    HarfBuzzShaper shaper(face);
    Batched2DRenderer renderer(device, *pipeline, *white);
    TextRenderer text(device, rasterizer, {256, 256, 4, 1});
    TextRenderer sync_text(device, sync_rasterizer, {256, 256, 4, 1});
    TextStyle style;
    style.font_size = 16.0f;
    const ShapedRun run = shaper.shape("Hello, prewarm 0123", style);

    auto draw = [&](TextRenderer& renderer_text) {
        renderer_text.begin_frame();
        auto commands = device.create_command_list();
        renderer.begin(*commands, *target, tessera::rendering::rhi::RenderTargetDesc{{256, 64}});
        renderer_text.draw(renderer, run, 4.0f, 40.0f, 16, rgba8(255, 255, 255));
        renderer.end();
        commands->submit();
    };
    draw(text);
    draw(sync_text);
    ManualIoExecutor executor;

    ok &= expect(text.begin_render_scale_step(2.0f, executor), "the step is accepted");
    ok &= expect(text.render_scale_step_pending() && text.render_scale() == 1.0f,
                 "the old scale stays active while the step is pending");
    ok &= expect(executor.stats().executed == 0 && !text.poll_render_scale_step(),
                 "nothing is rasterized or switched on the calling thread");
    const std::uint64_t misses = text.atlas().stats().misses;
    draw(text);
    ok &= expect(text.atlas().stats().misses == misses,
                 "drawing while pending keeps using the cached old-scale glyphs");
    ok &= expect(text.begin_render_scale_step(2.0f, executor) && executor.stats().posted == 1,
                 "asking for the pending step again posts nothing");
    static_cast<void>(executor.run_one());
    ok &= expect(!text.poll_render_scale_step(), "not switched before the completion");
    static_cast<void>(executor.run_completion_one());
    ok &= expect(text.poll_render_scale_step() && text.render_scale() == 2.0f &&
                     !text.render_scale_step_pending(),
                 "the poll after the completion switches the step");

    // Rendering the run at the new scale rasterizes nothing it prewarmed, and each glyph equals
    // the one a synchronous scale change produces.
    sync_text.set_render_scale(2.0f);
    draw(text);
    draw(sync_text);
    ok &=
        expect(text.atlas().stats().misses == 0, "the first draw at the new scale misses nothing");
    bool all_same = true;
    for (const auto& glyph : run.glyphs) {
        const GlyphAtlasEntry* a = text.atlas().find_glyph(glyph.id, 32, glyph.face, 0);
        const GlyphAtlasEntry* b = sync_text.atlas().find_glyph(glyph.id, 32, glyph.face, 0);
        all_same &=
            a != nullptr && b != nullptr && same_glyph(text.atlas(), *a, sync_text.atlas(), *b);
    }
    ok &= expect(all_same, "every glyph equals the synchronous rasterization");

    // set_render_scale abandons a pending step.
    ok &= expect(text.begin_render_scale_step(1.0f, executor) && text.render_scale_step_pending(),
                 "a step back is pending");
    text.set_render_scale(1.0f);
    static_cast<void>(executor.run(4));
    static_cast<void>(executor.run_completions(4));
    ok &= expect(!text.render_scale_step_pending() && !text.poll_render_scale_step() &&
                     text.render_scale() == 1.0f,
                 "a synchronous scale change abandons the pending step");
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "text_prewarm: missing font path\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "text_prewarm: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }
    GlyphRasterizer rasterizer(*loaded);
    bool ok = atlas_level(rasterizer);
    ok &= real_thread(rasterizer);
    ok &= renderer_level(*loaded);
    if (!ok) {
        std::cerr << "text_prewarm: FAILED\n";
        return 2;
    }
    std::cout << "text_prewarm: ok\n";
    return 0;
}
