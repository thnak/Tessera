// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace tessera::ui::paint {
namespace {

constexpr float kMinPixelSize = 1.0f;
constexpr float kMaxPixelSize = 4096.0f;

} // namespace

Batched2DSink::Batched2DSink(rendering::gpu::Batched2DRenderer& renderer,
                             text::TextRenderer* text_renderer, text::TextShaper* shaper)
    : renderer_(&renderer),
      text_renderer_(text_renderer),
      shaper_(shaper) {}

void Batched2DSink::fill_rect(const rhi::Rect& rect, const rhi::Color& color) {
    renderer_->push_rect(rect, color);
}

void Batched2DSink::fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                      float radius) {
    renderer_->push_rounded_rect(rect, color, radius);
}

void Batched2DSink::push_clip(const RoundedClip& clip) {
    renderer_->push_clip(rendering::gpu::clip::RoundedRect{clip.rect, clip.top_left, clip.top_right,
                                                           clip.bottom_right, clip.bottom_left});
}

void Batched2DSink::pop_clip() {
    renderer_->pop_clip();
}

void Batched2DSink::begin_text_frame() {
    if (text_renderer_ != nullptr) {
        text_renderer_->begin_frame();
    }
}

void Batched2DSink::draw_text(const TextDraw& run) {
    if (run.text.empty()) {
        return;
    }
    if (text_renderer_ == nullptr || shaper_ == nullptr) {
        throw std::runtime_error(
            "Batched2DSink::draw_text: no TextRenderer/TextShaper configured for this sink");
    }

    text::TextStyle style;
    style.font_size = run.font_size;
    style.font_weight = run.font_weight;
    const text::ShapedRun shaped = shaper_->shape(run.text, style);
    if (shaped.glyphs.empty()) {
        return;
    }

    const float size = std::clamp(run.font_size, kMinPixelSize, kMaxPixelSize);
    const auto pixel_size = static_cast<std::uint16_t>(std::lround(size));
    // Top-align the run in the node box; the shaper supplies the pixel ascent.
    const float baseline = run.rect.y + shaped.ascent;
    // TextRenderer pushes textured quads through the same renderer: a texture change flushes the
    // current rect batch, and the active scissor/rounded-clip state applies to the glyphs.
    text_renderer_->draw(*renderer_, shaped, run.rect.x, baseline, pixel_size, run.color);
}

} // namespace tessera::ui::paint
