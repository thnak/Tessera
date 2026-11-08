// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/animation/loop_bridge.hpp"

namespace tessera::ui::animation {

AnimationTickResult tick_animations(AnimationRuntime& runtime, runtime::RuntimeLoop& loop,
                                    double dt) {
    AnimationTickResult result = runtime.tick(dt);
    if (result.keep_awake || !result.finished.empty()) {
        loop.request_frame();
    }
    return result;
}

} // namespace tessera::ui::animation
