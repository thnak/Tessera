// SPDX-License-Identifier: MIT
//
// Shared scaffolding for the Vulkan-free CPU paint corpus (`cpu_wide_golden_tests.cpp`): the
// canvas, the paint sink, the stub style provider, the pixel helpers and the scene colours. Split
// out of the test so each scene reads as a scene and the test file stays under the source-size
// limit.

#pragma once

#include "cpu_paint_sink.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tessera::poc::batched2d::cpu_wide_support {

using tessera::rendering::rhi::rgba8;
using tessera::rendering::software::CpuCanvas;
using tessera::ui::LinearGradient;
using tessera::ui::LinearGradientStop;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::RadialGradient;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

inline constexpr std::uint32_t kWidth = 320;
inline constexpr std::uint32_t kHeight = 200;

using Pixel = std::array<std::uint8_t, 4>;

inline bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "cpu-wide-golden: " << message << "\n";
    }
    return condition;
}

inline Pixel pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y) {
    const std::size_t index = (static_cast<std::size_t>(y) * canvas.width() + x) * 4;
    return {canvas.pixels()[index], canvas.pixels()[index + 1], canvas.pixels()[index + 2],
            canvas.pixels()[index + 3]};
}

inline bool is_pixel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y,
                     const Pixel& expected) {
    return pixel(canvas, x, y) == expected;
}

// Channel order is R, G, B, A; returning the raw byte keeps gradient assertions about dominance
// instead of pinning a libm-dependent ramp value.
inline std::uint8_t channel(const CpuCanvas& canvas, std::uint32_t x, std::uint32_t y,
                            std::size_t index) {
    return pixel(canvas, x, y)[index];
}

inline std::vector<std::uint8_t> bytes(const CpuCanvas& canvas) {
    return {canvas.pixels().begin(), canvas.pixels().end()};
}

class StubStyles final : public StyleProvider {
public:
    void set(tessera::ui::NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }

    void add(tessera::ui::NodeId id, const char* property, const char* value) {
        styles_[id].set(property, value);
    }

    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<tessera::ui::NodeId, ComputedStyle> styles_;
};

inline ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

inline Node& add_node(Node& parent, tessera::ui::NodeId id,
                      const tessera::rendering::rhi::Rect& bounds) {
    auto owner = std::make_unique<Node>(id);
    owner->set_bounds(bounds);
    return parent.add_child(std::move(owner));
}

inline void clip_children(StubStyles& styles, tessera::ui::NodeId id) {
    styles.add(id, "overflow-x", "hidden");
    styles.add(id, "overflow-y", "hidden");
}

inline PaintData gradient_payload(float angle_degrees,
                                  std::initializer_list<LinearGradientStop> stops) {
    LinearGradient gradient;
    gradient.angle_degrees = angle_degrees;
    gradient.stop_count = stops.size();
    std::size_t index = 0;
    for (const LinearGradientStop& stop : stops) {
        gradient.stops[index++] = stop;
    }
    PaintData data;
    data.gradient = gradient;
    return data;
}

// A radial payload (issue #613). Centre and radii are fractions of the node box, so a scene reads
// in the same terms a stylesheet would. `repeating` expands the authored ramp over the radius.
inline PaintData radial_payload(float center_x, float center_y, float radius_x, float radius_y,
                                std::initializer_list<LinearGradientStop> stops,
                                bool repeating = false) {
    RadialGradient gradient;
    gradient.shape = ui::GradientShape::Radial;
    gradient.radial.center_x = center_x;
    gradient.radial.center_y = center_y;
    gradient.radial.radius_x = radius_x;
    gradient.radial.radius_y = radius_y;
    gradient.repeating = repeating;
    gradient.stop_count = stops.size();
    std::size_t index = 0;
    for (const LinearGradientStop& stop : stops) {
        gradient.stops[index++] = stop;
    }
    PaintData data;
    data.gradient = gradient;
    return data;
}

// Scene colours. Opaque fills round-trip every byte exactly through `CpuCanvas::blend`.
inline constexpr Pixel kRoot{11, 17, 32, 255};             // #0B1120
inline constexpr Pixel kCard{30, 41, 59, 255};             // #1E293B
inline constexpr Pixel kCardAlt{17, 24, 39, 255};          // #111827
inline constexpr Pixel kInner{226, 232, 240, 255};         // #E2E8F0
inline constexpr Pixel kAccent{56, 189, 248, 255};         // #38BDF8
inline constexpr Pixel kBorderRight{244, 114, 182, 255};   // #F472B6
inline constexpr Pixel kBorderBottom{250, 204, 21, 255};   // #FACC15
inline constexpr Pixel kBorderLeft{34, 197, 94, 255};      // #22C55E
inline constexpr Pixel kBorderUniform{167, 139, 250, 255}; // #A78BFA
inline constexpr Pixel kMiddle{51, 65, 85, 255};           // #334155
inline constexpr Pixel kLeaf{239, 68, 68, 255};            // #EF4444
inline constexpr Pixel kOrange{249, 115, 22, 255};         // #F97316
inline constexpr Pixel kSlate{71, 85, 105, 255};           // #475569

inline CpuCanvas make_canvas() {
    CpuCanvas canvas(kWidth, kHeight);
    canvas.clear(rgba8(kRoot[0], kRoot[1], kRoot[2]));
    return canvas;
}

} // namespace tessera::poc::batched2d::cpu_wide_support
