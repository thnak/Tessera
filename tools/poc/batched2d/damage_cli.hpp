// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/redraw_plan.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string_view>
#include <system_error>
#include <vector>

namespace tessera::poc::batched2d {

inline bool parse_damage_rect(std::string_view text, rendering::rhi::Rect& rect) {
    std::array<std::string_view, 4> tokens{};
    std::size_t begin = 0;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::size_t comma = text.find(',', begin);
        const bool last_token = i + 1 == tokens.size();
        if (last_token) {
            if (comma != std::string_view::npos) {
                return false;
            }
            tokens[i] = text.substr(begin);
        } else {
            if (comma == std::string_view::npos) {
                return false;
            }
            tokens[i] = text.substr(begin, comma - begin);
            begin = comma + 1;
        }
    }

    std::array<float, 4> values{};
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const char* first = tokens[i].data();
        const char* end = first + tokens[i].size();
        if (first == end) {
            return false;
        }
        const auto result = std::from_chars(first, end, values[i]);
        if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(values[i])) {
            return false;
        }
    }

    rect = rendering::rhi::Rect{values[0], values[1], values[2], values[3]};
    return true;
}

inline rendering::RedrawPlan build_damage_plan(const std::vector<rendering::rhi::Rect>& rects,
                                               rendering::rhi::Extent2D extent) {
    rendering::DamageAccumulator damage(rendering::DamageConfig{extent, 16});
    for (const rendering::rhi::Rect& rect : rects) {
        damage.add(rect);
    }

    rendering::RedrawPlan plan = rendering::make_redraw_plan(damage);
    if (rendering::should_full_redraw(plan, extent)) {
        plan.full_redraw = true;
        plan.scissors.clear();
        plan.scissors.push_back(rendering::rhi::Scissor{0, 0, extent.width, extent.height});
    }
    return plan;
}

inline void print_plan_summary(const rendering::RedrawPlan& plan) {
    std::cout << "plan: " << (plan.full_redraw ? "full_redraw" : "partial")
              << " scissors=" << plan.scissors.size() << " regions=";
    if (plan.full_redraw) {
        std::cout << "full";
    } else {
        for (std::size_t i = 0; i < plan.scissors.size(); ++i) {
            const rendering::rhi::Scissor& scissor = plan.scissors[i];
            if (i != 0) {
                std::cout << ';';
            }
            std::cout << scissor.x << ',' << scissor.y << ',' << scissor.width << 'x'
                      << scissor.height;
        }
    }
    std::cout << "\n";
}

// The POC renders a single synthetic frame, so it keeps the Clear load op. A real
// partial redraw begins the target with LoadOp::Load to preserve undamaged pixels.
inline void begin_with_damage(rendering::gpu::Batched2DRenderer& renderer,
                              rendering::rhi::CommandList& commands,
                              const rendering::rhi::RenderTarget& target,
                              const rendering::rhi::RenderTargetDesc& desc,
                              const std::vector<rendering::rhi::Rect>& damage_rects) {
    if (damage_rects.empty()) {
        renderer.begin(commands, target, desc);
        return;
    }
    const rendering::RedrawPlan plan = build_damage_plan(damage_rects, desc.extent);
    print_plan_summary(plan);
    renderer.begin_damaged(commands, target, desc, plan);
}

} // namespace tessera::poc::batched2d
