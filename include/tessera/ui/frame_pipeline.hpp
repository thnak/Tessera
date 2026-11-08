// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/animation/runtime.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"

namespace tessera::runtime {
class RuntimeLoop;
}

namespace tessera::ui::frame {

struct FramePipelineConfig {
    float dpi_scale = 1.0f;
    float damage_margin = 0.0f;
    rendering::RedrawPolicy redraw_policy{};
};

struct FrameTick {
    animation::AnimationTickResult animation;
    bool needs_style = false;
    bool needs_layout = false;
};

struct FrameRender {
    layout::DamagePassResult damage;
    rendering::RedrawPlan redraw_plan;
    bool full_redraw = false;
    bool should_render = false;
};

class FramePipeline {
public:
    FramePipeline(animation::AnimationRuntime& animations, runtime::RuntimeLoop& loop,
                  layout::DamageTracker& damage_tracker, rendering::DamageAccumulator& damage,
                  DirtyPass& dirty, FramePipelineConfig config = {});

    FrameTick tick(Node& root, double dt);
    FrameRender finish_frame(Node& root);

private:
    animation::AnimationRuntime* animations_ = nullptr;
    runtime::RuntimeLoop* loop_ = nullptr;
    layout::DamageTracker* damage_tracker_ = nullptr;
    rendering::DamageAccumulator* damage_ = nullptr;
    DirtyPass* dirty_ = nullptr;
    FramePipelineConfig config_;
};

} // namespace tessera::ui::frame
