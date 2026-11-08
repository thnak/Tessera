// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/runtime.hpp"

namespace tessera::runtime {
class RuntimeLoop;
}

namespace tessera::ui::animation {

AnimationTickResult tick_animations(AnimationRuntime& runtime, runtime::RuntimeLoop& loop,
                                    double dt);

} // namespace tessera::ui::animation
