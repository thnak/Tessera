// SPDX-License-Identifier: MIT
//
// One benchmark run: a fresh reference scene, the frame driver, and the null-RHI batched renderer
// that records the paint into draw lists. Every run rebuilds the fixture so the first frame is
// genuinely cold (cold selector tree, cold layout scheduler, cold paint cache). Rebuilds are not
// timed; only the style/layout/paint work between `measure()`'s timestamps is.

#pragma once

#include "benchmark_driver.hpp"
#include "benchmark_scene.hpp"
#include "benchmark_sink.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/image/image_texture_cache.hpp"
#include "tessera/rendering/rhi/device.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"

#include <cstddef>
#include <memory>

namespace tessera::benchmark {

struct PaintOutcome {
    double ms = 0.0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
};

// One measured run. Times are per scenario; counters are from the full paint.
struct Measurement {
    double first_style_layout_ms = 0.0;
    double full_paint_ms = 0.0;
    double layout_paint_text_ms = 0.0;
    double layout_paint_container_ms = 0.0;
    std::size_t damage_rects_full = 0;
    std::size_t damage_rects_text = 0;
    std::size_t damage_rects_container = 0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
};

// Per-run maxima, accumulated by the caller across runs (the counters are deterministic, the times
// are summarized as median/p95).
struct Totals {
    // Filled by main from the collected samples.
    std::size_t damage_rects = 0;
    std::size_t draw_calls = 0;
    std::size_t quads = 0;
    std::size_t primitives = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t paint_nodes = 0;
};

class Fixture final {
public:
    Fixture();
    ~Fixture();

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    [[nodiscard]] Measurement measure();
    [[nodiscard]] std::size_t nodes() const noexcept {
        return targets_.nodes;
    }

private:
    [[nodiscard]] PaintOutcome paint(bool force);
    void edit_text();
    void edit_container();

    std::unique_ptr<ui::Node> root_;
    SceneBuilder builder_;
    SceneTargets targets_{};
    std::unique_ptr<rendering::rhi::Device> device_;
    std::unique_ptr<rendering::rhi::RenderTarget> target_;
    std::unique_ptr<rendering::rhi::Texture> white_;
    std::unique_ptr<rendering::rhi::Pipeline> pipeline_;
    std::unique_ptr<rendering::rhi::CommandList> commands_;
    std::unique_ptr<rendering::gpu::Batched2DRenderer> renderer_;
    ui::assets::AssetBundle bundle_;
    rendering::image::ImageDecodeCache decoded_;
    rendering::image::ImageTextureCache textures_;
    ui::paint::Batched2DSink batched_;
    CountingSink sink_;
    std::unique_ptr<BenchmarkDriver> driver_;
};

} // namespace tessera::benchmark
