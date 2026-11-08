// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/dirty.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace tessera::ui::animation {

using AnimationId = std::uint64_t;
using AnimationCallback = std::function<void(AnimationId)>;

struct AnimationInput {
    std::uint64_t target = 0;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    AnimationCallback on_complete;
};

struct AnimationEntry {
    AnimationId id = 0;
    std::uint64_t target = 0;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    double elapsed = 0.0;
    AnimationCallback on_complete;
};

struct AnimationSample {
    AnimationId id = 0;
    std::uint64_t target = 0;
    std::vector<style::Declaration> declarations;
};

struct AnimationTickResult {
    std::vector<AnimationSample> samples;
    std::vector<AnimationId> finished;
    Dirty dirty = Dirty::None;
    bool keep_awake = false;
};

class AnimationRuntime {
public:
    AnimationId start(AnimationInput input);

    [[nodiscard]] AnimationTickResult tick(double dt);
    bool stop(AnimationId id);
    void clear();

    [[nodiscard]] std::size_t active() const {
        return entries_.size();
    }
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }
    [[nodiscard]] const std::vector<AnimationEntry>& entries() const {
        return entries_;
    }

private:
    std::vector<AnimationEntry> entries_;
    AnimationId next_id_ = 1;
};

} // namespace tessera::ui::animation
