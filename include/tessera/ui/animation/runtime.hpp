// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/animation/keyframes.hpp"
#include "tessera/ui/dirty.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::animation {

using AnimationId = std::uint64_t;
using AnimationCallback = std::function<void(AnimationId)>;
using AnimationListenerId = std::uint64_t;

enum class AnimationKind : std::uint8_t { Animation, Transition };

// animationend / transitionend analogue. `name` is the @keyframes name for an animation and the
// transitioned property for a transition; `elapsed` is the active time in seconds (delay excluded).
struct AnimationEvent {
    AnimationId id = 0;
    std::uint64_t target = 0;
    AnimationKind kind = AnimationKind::Animation;
    std::string name;
    std::string property;
    double elapsed = 0.0;
};

using AnimationEventListener = std::function<void(const AnimationEvent&)>;

struct AnimationInput {
    std::uint64_t target = 0;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    AnimationCallback on_complete;
    std::string property;
    bool paused = false;
    AnimationKind kind = AnimationKind::Animation;
};

struct AnimationEntry {
    AnimationId id = 0;
    std::uint64_t target = 0;
    style::KeyframeBlock keyframes;
    KeyframeTiming timing;
    double elapsed = 0.0;
    AnimationCallback on_complete;
    std::string property;
    bool paused = false;
    AnimationKind kind = AnimationKind::Animation;
    // Computed once at start(): every animated property is paint-only and non-essential
    // (property_dirty.hpp). Only such entries freeze under suspend_non_essential().
    bool non_essential = false;
};

struct AnimationSample {
    AnimationId id = 0;
    std::uint64_t target = 0;
    std::vector<style::Declaration> declarations;
};

struct AnimationTickResult {
    std::vector<AnimationSample> samples;
    std::vector<AnimationId> finished;
    Dirty dirty = Dirty::Clean;
    bool keep_awake = false;
};

struct AnimationNameState {
    std::size_t active = 0;
    std::size_t settled = 0;
};

class AnimationRuntime {
public:
    AnimationId start(AnimationInput input);

    [[nodiscard]] AnimationTickResult tick(double dt);
    bool pause(AnimationId id);
    bool resume(AnimationId id);
    std::size_t pause_target(std::uint64_t target);
    std::size_t pause_target_name(std::uint64_t target, std::string_view name);
    std::size_t resume_target(std::uint64_t target);
    std::size_t resume_target_name(std::uint64_t target, std::string_view name);
    std::size_t stop_target(std::uint64_t target);
    std::size_t stop_target_name(std::uint64_t target, std::string_view name);
    bool restart_target_name(std::uint64_t target, std::string_view name);
    bool restart(AnimationId id);
    bool stop(AnimationId id);
    std::size_t stop_target_property(std::uint64_t target, std::string_view property);
    void clear();

    // Listeners learn about natural completions only (never stop/clear/supersession). tick()
    // queues events; dispatch_events() delivers them outside the tick mutation, so a listener may
    // freely start, stop or restart animations or add/remove listeners. Returns events delivered.
    AnimationListenerId add_event_listener(AnimationEventListener listener);
    bool remove_event_listener(AnimationListenerId id);
    std::size_t dispatch_events();
    [[nodiscard]] std::size_t pending_events() const {
        return pending_events_.size();
    }

    [[nodiscard]] AnimationNameState target_name_state(std::uint64_t target,
                                                       std::string_view name) const;

    [[nodiscard]] std::size_t active() const {
        return entries_.size();
    }
    [[nodiscard]] std::size_t settled() const {
        return fills_.size();
    }
    [[nodiscard]] bool empty() const {
        return entries_.empty();
    }
    // Adaptive quality's last ladder position (#147): non-essential entries stop advancing, keep
    // their elapsed time (so they resume where they stopped) and no longer keep the loop awake. It
    // is independent of `paused`, so a recovery never resumes an animation the app paused itself.
    void set_suspend_non_essential(bool suspend) {
        suspend_non_essential_ = suspend;
    }
    [[nodiscard]] bool suspend_non_essential() const {
        return suspend_non_essential_;
    }
    // Entries currently frozen by the suspension (diagnostics and tests).
    [[nodiscard]] std::size_t suspended() const;
    void set_reduced_motion(bool enabled) {
        reduced_motion_ = enabled;
    }
    [[nodiscard]] bool reduced_motion() const {
        return reduced_motion_;
    }
    [[nodiscard]] const std::vector<AnimationEntry>& entries() const {
        return entries_;
    }
    [[nodiscard]] const std::vector<AnimationEntry>& settled_entries() const {
        return fills_;
    }

private:
    std::vector<AnimationEntry> entries_;
    std::vector<AnimationEntry> fills_;
    std::vector<AnimationEvent> pending_events_;
    std::vector<std::pair<AnimationListenerId, AnimationEventListener>> listeners_;
    AnimationId next_id_ = 1;
    AnimationListenerId next_listener_id_ = 1;
    bool reduced_motion_ = false;
    bool suspend_non_essential_ = false;
    [[nodiscard]] bool frozen(const AnimationEntry& entry) const {
        return entry.paused || (suspend_non_essential_ && entry.non_essential);
    }
};

} // namespace tessera::ui::animation
