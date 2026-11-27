// SPDX-License-Identifier: MIT

// Gradient fills of Batched2DSink (docs/design/gpu/gradients.md, ADR-076).
//
// A gradient layer is ONE quad evaluated exactly per pixel by the fragment shader. The banded
// emission the engine used before (64 solid bands for a linear gradient, a 32 x 32 grid for a
// radial one, 64 x 64 for a conic one) stays only as the capability fallback: a device that does
// not report `supports_analytic_gradients()`, a frame pipeline without the record buffer, or a
// frame whose gradient table is full. The fallback draws exactly what the engine drew before, so a
// device that cannot run the shader loses fidelity, never output.

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/gradient_fill.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace tessera::ui::paint {
namespace {

// Pushes the node's border-rounded clip for the life of one fill. A square clip is a no-op: the
// quad's own edges are the shape.
class RoundedClipScope {
public:
    RoundedClipScope(rendering::gpu::Batched2DRenderer& renderer, const RoundedClip& clip)
        : renderer_(renderer),
          active_(clip.rounded()) {
        if (active_) {
            renderer_.push_clip(rendering::gpu::clip::RoundedRect{
                clip.rect, clip.top_left, clip.top_right, clip.bottom_right, clip.bottom_left});
        }
    }
    ~RoundedClipScope() {
        if (active_) {
            renderer_.pop_clip();
        }
    }
    RoundedClipScope(const RoundedClipScope&) = delete;
    RoundedClipScope& operator=(const RoundedClipScope&) = delete;

private:
    rendering::gpu::Batched2DRenderer& renderer_;
    bool active_;
};

// The banded fallback of a linear gradient: 64 dominant-axis bands sampled with the shared
// encoded-space ramp. Exact for 0/90/180/270 degrees; other angles are a banded approximation. The
// bands abut exactly (band_edge): no overlap, so a translucent stop is composited once.
void paint_linear_bands(rendering::gpu::Batched2DRenderer& renderer, const GradientDraw& draw) {
    const float radians = draw.angle_degrees * 3.14159265358979323846f / 180.0f;
    const float dx = std::sin(radians);
    const float dy = -std::cos(radians);
    const float span =
        std::max(draw.rect.width * std::abs(dx) + draw.rect.height * std::abs(dy), 1.0f);
    constexpr int kBands = 64;
    if (std::abs(dx) >= std::abs(dy)) {
        const float band = draw.rect.width / static_cast<float>(kBands);
        for (int index = 0; index < kBands; ++index) {
            const float x = draw.rect.x + (static_cast<float>(index) + 0.5f) * band;
            const float t = 0.5f + ((x - (draw.rect.x + draw.rect.width * 0.5f)) * dx) / span;
            const float left = band_edge(draw.rect.x, draw.rect.width, index, kBands);
            const float right = band_edge(draw.rect.x, draw.rect.width, index + 1, kBands);
            renderer.push_rect({left, draw.rect.y, right - left, draw.rect.height},
                               sample_linear_gradient(draw, t));
        }
    } else {
        const float band = draw.rect.height / static_cast<float>(kBands);
        for (int index = 0; index < kBands; ++index) {
            const float y = draw.rect.y + (static_cast<float>(index) + 0.5f) * band;
            const float t = 0.5f + ((y - (draw.rect.y + draw.rect.height * 0.5f)) * dy) / span;
            const float top = band_edge(draw.rect.y, draw.rect.height, index, kBands);
            const float bottom = band_edge(draw.rect.y, draw.rect.height, index + 1, kBands);
            renderer.push_rect({draw.rect.x, top, draw.rect.width, bottom - top},
                               sample_linear_gradient(draw, t));
        }
    }
}

} // namespace

void Batched2DSink::fill_linear_gradient(const GradientDraw& draw) {
    if (draw.stop_count == 0 || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    const RoundedClipScope clip(*renderer_, draw.clip);
    const FillStops stops(draw.stops.data(), draw.stop_count);
    if (!renderer_->push_gradient(linear_fill(draw, stops))) {
        paint_linear_bands(*renderer_, draw);
    }
}

// A radial gradient has no dominant axis to band along, so the fallback tessellates it into a
// bounded grid of solid cells by the shared `tessellate_radial_gradient` (paint_sink.hpp).
void Batched2DSink::fill_radial_gradient(const RadialGradientDraw& draw) {
    if (draw.stop_count == 0 || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f ||
        draw.radius_x <= 0.0f || draw.radius_y <= 0.0f) {
        return; // no ramp to place: the same early-out tessellate_radial_gradient takes
    }
    const RoundedClipScope clip(*renderer_, draw.clip);
    const FillStops stops(draw.stops.data(), draw.stop_count);
    if (!renderer_->push_gradient(radial_fill(draw, stops))) {
        tessellate_radial_gradient(
            draw, [this](const RadialBand& band) { renderer_->push_rect(band.rect, band.color); });
    }
}

void Batched2DSink::fill_conic_gradient(const ConicGradientDraw& draw) {
    if (draw.stop_count == 0 || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    const RoundedClipScope clip(*renderer_, draw.clip);
    const FillStops stops(draw.stops.data(), draw.stop_count);
    if (!renderer_->push_gradient(conic_fill(draw, stops))) {
        tessellate_conic_gradient(
            draw, [this](const ConicBand& band) { renderer_->push_rect(band.rect, band.color); });
    }
}

} // namespace tessera::ui::paint
