// SPDX-License-Identifier: MIT
//
// Draw-list recording sink for the CPU benchmark (issue #241). It forwards the raster geometry
// (fills, gradients, images, vectors) to a null-RHI `Batched2DSink` so the measurement includes
// real batch/command recording, and counts every draw-list command the paint pass emits. Text runs
// are counted but not rasterized: the benchmark runs without a font (the deterministic text stub
// supplies layout metrics), so a text run is one recorded draw-list item.

#pragma once

#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <cstddef>

namespace tessera::benchmark {

namespace rhi = tessera::rendering::rhi;

// Counts per full paint. `primitives()` is the draw-list item count (one per emitted command);
// the batched renderer's own `draw_calls()`/`vertex_count()` cover the forwarded geometry.
struct DrawListCounts {
    std::size_t fills = 0;
    std::size_t rounded_fills = 0;
    std::size_t gradients = 0;
    std::size_t text_runs = 0;
    std::size_t images = 0;
    std::size_t vectors = 0;
    std::size_t clips = 0;
    std::size_t transforms = 0;
    // Glass backdrop composites (issue #244). Forwarded to the batched sink, so the benchmark pays
    // the real recording cost (region copy + two blur passes + composite) on the null RHI.
    std::size_t backdrops = 0;

    [[nodiscard]] std::size_t primitives() const noexcept {
        return fills + rounded_fills + gradients + text_runs + images + vectors;
    }

    void reset() noexcept {
        *this = DrawListCounts{};
    }
};

class CountingSink final : public ui::paint::PaintSink {
public:
    explicit CountingSink(ui::paint::Batched2DSink& batched)
        : batched_(&batched) {}

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        ++counts_.fills;
        batched_->fill_rect(rect, color);
    }
    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        ++counts_.rounded_fills;
        batched_->fill_rounded_rect(rect, color, radius);
    }
    void fill_linear_gradient(const ui::paint::GradientDraw& draw) override {
        ++counts_.gradients;
        batched_->fill_linear_gradient(draw);
    }
    void push_clip(const ui::paint::RoundedClip& clip) override {
        ++counts_.clips;
        batched_->push_clip(clip);
    }
    void pop_clip() override {
        batched_->pop_clip();
    }
    [[nodiscard]] bool supports_transform() const override {
        return batched_->supports_transform();
    }
    void push_transform(const rhi::Mat3& transform) override {
        ++counts_.transforms;
        batched_->push_transform(transform);
    }
    void pop_transform() override {
        batched_->pop_transform();
    }
    void draw_text(const ui::paint::TextDraw& /*run*/) override {
        ++counts_.text_runs;
    }
    void draw_vector(const ui::paint::VectorDraw& draw) override {
        ++counts_.vectors;
        batched_->draw_vector(draw);
    }
    void draw_image(const ui::paint::ImageDraw& draw) override {
        ++counts_.images;
        batched_->draw_image(draw);
    }
    [[nodiscard]] bool supports_backdrop() const override {
        return batched_->supports_backdrop();
    }
    void draw_backdrop(const ui::paint::BackdropDraw& draw) override {
        ++counts_.backdrops;
        batched_->draw_backdrop(draw);
    }

    [[nodiscard]] const DrawListCounts& counts() const noexcept {
        return counts_;
    }
    void reset() noexcept {
        counts_.reset();
    }

private:
    ui::paint::Batched2DSink* batched_ = nullptr;
    DrawListCounts counts_;
};

} // namespace tessera::benchmark
