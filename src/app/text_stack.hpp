// SPDX-License-Identifier: MIT
//
// The font/text stack a Batched2DSink needs, shared by the Vulkan window driver, the windowless
// frame capture and the web app driver (ADR-046). It depends on the RHI seam and the text libraries
// only, never on a backend, so every driver builds it the same way. Not a public header.

#pragma once

#include "tessera/rendering/rhi/device.hpp"
#include "tessera/runtime/async.hpp"
#include "tessera/runtime/worker_io_executor.hpp"
#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_text_measurer.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/weighted_shaper.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tessera::app::detail {

namespace rhi = tessera::rendering::rhi;

// The loaded font faces plus the rasterizer/shaper/TextRenderer trio. `font_path` is the primary
// face; each `extra_font_paths` entry is another face of the family (a bold file, say), picked per
// run by CSS `font-weight` from the weight each file declares (`WeightedShaper`). Member order
// matters: the renderer borrows the rasterizers, so the faces are constructed first and never moved
// afterwards.
class TextStack final {
public:
    // `async_scale` false skips the worker executor: a single-threaded build (the web app driver,
    // ADR-046 item 6) has no thread to start, so a scale step runs synchronously instead.
    TextStack(rhi::Device& device, const std::string& font_path,
              const std::vector<std::string>& extra_font_paths = {}, bool async_scale = true)
        : face(load(font_path)) {
        faces.push_back(face);
        for (const std::string& extra : extra_font_paths) {
            faces.push_back(load(extra));
        }
        std::vector<ui::text::GlyphRasterizer*> raster_stack;
        rasterizers.reserve(faces.size());
        for (const ui::text::FontFace& loaded : faces) {
            rasterizers.push_back(std::make_unique<ui::text::GlyphRasterizer>(loaded));
            raster_stack.push_back(rasterizers.back().get());
        }
        rasterizer = rasterizers.front().get();
        shaper = std::make_unique<ui::text::WeightedShaper>(faces);
        renderer = std::make_unique<ui::text::TextRenderer>(
            device, std::move(raster_stack), ui::text::GlyphAtlasConfig{1024, 1024, 2, 1});
        // Layout and paint must measure with the face that is actually drawn (issue #314): the
        // deterministic stub advance is a fixed 0.5 em per code point and does not track the font,
        // so a content-sized box would be narrower or wider than its painted ink by a
        // string-dependent amount. These two seams and the glyph renderer now share one face.
        // One in-flight step and one worker: a newer step abandons the older one, so a second slot
        // would only hold a job nobody installs.
        if (async_scale) {
            scale_executor = std::make_unique<runtime::WorkerIoExecutor>(
                scale_completions, runtime::WorkerIoExecutor::Config{1, 1, 4});
        }
        measurer = std::make_shared<ui::text::FontTextMeasurer>(faces);
        paint_metrics = std::make_unique<ui::paint::FontPaintTextMetrics>(faces, kPaintMetricsId);
    }

    // Abandons a pending step first so the join in the executor's destructor ends at the worker's
    // next glyph instead of after the full rasterization.
    // Applies `policy` without rasterizing on the calling thread; falls back to the synchronous
    // path only when the worker refuses the step.
    void apply_policy(const ui::scale::AaPolicy& policy) {
        if (scale_executor == nullptr || !renderer->begin_aa_policy_step(policy, *scale_executor)) {
            renderer->set_aa_policy(policy);
        }
    }

    // Between frames: runs the worker's completion (bounded, never waits) and lands a finished
    // step. True when the active policy changed, which stales every quad batched at the old scale.
    // `pending` reports a step still in flight so the loop keeps ticking until it lands.
    bool poll_scale_step(bool& pending) {
        static_cast<void>(scale_completions.run(4));
        const bool landed = renderer->poll_render_scale_step();
        pending = renderer->render_scale_step_pending();
        return landed;
    }

    ~TextStack() {
        if (renderer != nullptr) {
            renderer->cancel_render_scale_step();
        }
        scale_executor.reset();
    }
    TextStack(const TextStack&) = delete;
    TextStack& operator=(const TextStack&) = delete;

    ui::text::FontFace face;
    std::vector<ui::text::FontFace> faces; // primary first, then the extras
    std::vector<std::unique_ptr<ui::text::GlyphRasterizer>> rasterizers;
    ui::text::GlyphRasterizer* rasterizer = nullptr; // the primary face's
    std::unique_ptr<ui::text::WeightedShaper> shaper;
    std::unique_ptr<ui::text::TextRenderer> renderer;
    // The off-thread glyph scale step (issue #993). The queue and executor are declared after the
    // renderer so they die first: the executor joins its worker (no detached tasks) while the faces
    // and the renderer it prepares glyphs for are still alive, and the queue outlives the executor
    // that posts to it.
    runtime::CompletionQueue scale_completions{16};
    std::unique_ptr<runtime::WorkerIoExecutor> scale_executor;
    // The same face as a layout measurer and as paint's wrapping metrics source (issue #314): the
    // stub advance is a fixed 0.5 em per code point and does not track the font, so a content-sized
    // box disagrees with its painted ink by a string-dependent amount. A caller installs both
    // (HostConfig::text_measurer and PaintContext::text_metrics_source).
    //
    // Lifetime: both objects own their own `FontFace` copy, and `FontFace` holds a
    // `shared_ptr<FontFaceState>` - so the measurer keeps the FreeType face alive on its own and
    // the layout engine's `shared_ptr` to it is never dangling, even though `TextStack` is
    // destroyed (or replaced on device loss) after the `AppHost` that borrows the measurer. The
    // glyphs the sink draws come from `renderer`/`shaper` above, which live exactly as long as this
    // object and the sink that borrows them.
    std::shared_ptr<const ui::text::TextMeasurer> measurer;
    std::unique_ptr<ui::paint::FontPaintTextMetrics> paint_metrics;

    // Stable per-face id for the paint text-layout cache key; distinct from the stub's id 0 so stub
    // and real-face line boxes can never mix in one cache.
    static constexpr std::uint32_t kPaintMetricsId = 1;

private:
    static ui::text::FontFace load(const std::string& path) {
        if (path.empty()) {
            throw std::invalid_argument("tessera app window: a font path is required");
        }
        auto loaded = ui::text::FontFace::load_from_file(path);
        if (!loaded) {
            throw std::runtime_error("tessera app window: failed to load font " + path + ": " +
                                     loaded.error().message);
        }
        return *loaded;
    }
};

} // namespace tessera::app::detail
