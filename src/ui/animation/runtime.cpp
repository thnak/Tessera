// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/animation/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

constexpr double kReducedMotionOpacityDuration = 0.12;

bool is_opacity_only(const style::KeyframeBlock& keyframes) {
    bool found = false;
    for (const style::KeyframeStep& step : keyframes.steps) {
        for (const style::Declaration& declaration : step.declarations) {
            if (declaration.property.empty()) {
                continue;
            }
            found = true;
            if (declaration.property != "opacity") {
                return false;
            }
        }
    }
    return found;
}

void apply_reduced_motion(AnimationInput& input) {
    input.timing.delay = 0.0;
    input.timing.iterations = 1.0;
    input.timing.direction = Direction::Normal;
    if (is_opacity_only(input.keyframes)) {
        input.timing.duration = std::min(input.timing.duration, kReducedMotionOpacityDuration);
    } else {
        input.timing.duration = 0.0;
    }
}

bool is_finished(const AnimationEntry& entry) {
    if (!std::isfinite(entry.timing.iterations)) {
        return false;
    }
    if (entry.timing.duration <= 0.0) {
        return entry.elapsed >= entry.timing.delay;
    }
    const double total = entry.timing.delay + entry.timing.duration * entry.timing.iterations;
    return std::isfinite(total) && entry.elapsed >= total;
}

AnimationSample sample_entry(const AnimationEntry& entry, double at) {
    AnimationSample sample;
    sample.id = entry.id;
    sample.target = entry.target;
    sample.declarations = sample_keyframes(entry.keyframes, entry.timing, at);
    return sample;
}

} // namespace

AnimationId AnimationRuntime::start(AnimationInput input) {
    if (reduced_motion_) {
        apply_reduced_motion(input);
    }
    AnimationEntry entry;
    entry.id = next_id_++;
    entry.target = input.target;
    entry.keyframes = std::move(input.keyframes);
    entry.timing = input.timing;
    entry.on_complete = std::move(input.on_complete);
    entry.property = std::move(input.property);
    entry.paused = input.paused;
    entry.kind = input.kind;
    entry.non_essential = is_non_essential_animation(entry.keyframes);
    fills_.erase(std::remove_if(fills_.begin(), fills_.end(),
                                [&](const AnimationEntry& fill) {
                                    if (fill.target != entry.target) {
                                        return false;
                                    }
                                    if (!entry.keyframes.name.empty() &&
                                        fill.keyframes.name == entry.keyframes.name) {
                                        return true;
                                    }
                                    return !entry.property.empty() &&
                                           fill.property == entry.property;
                                }),
                 fills_.end());
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

AnimationTickResult AnimationRuntime::tick(double dt) {
    AnimationTickResult result;
    if (!std::isfinite(dt) || dt < 0.0) {
        result.keep_awake =
            std::any_of(entries_.begin(), entries_.end(),
                        [this](const AnimationEntry& entry) { return !frozen(entry); });
        return result;
    }

    std::vector<AnimationEntry> finished;
    for (AnimationEntry& entry : entries_) {
        if (frozen(entry)) {
            continue;
        }

        entry.elapsed += dt;
        if (!is_finished(entry)) {
            continue;
        }

        result.finished.push_back(entry.id);
        if (entry.timing.fill == FillMode::Forwards || entry.timing.fill == FillMode::Both) {
            entry.elapsed = entry.timing.delay + entry.timing.duration * entry.timing.iterations;
        }
        finished.push_back(std::move(entry));
    }

    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const AnimationEntry& entry) {
                                      return std::any_of(finished.begin(), finished.end(),
                                                         [&](const AnimationEntry& done) {
                                                             return done.id == entry.id;
                                                         });
                                  }),
                   entries_.end());

    struct FinishedCallback {
        AnimationId id = 0;
        AnimationCallback callback;
    };
    std::vector<FinishedCallback> callbacks;
    callbacks.reserve(finished.size());
    for (AnimationEntry& entry : finished) {
        const AnimationId id = entry.id;
        AnimationCallback callback = entry.on_complete;
        AnimationEvent event;
        event.id = id;
        event.target = entry.target;
        event.kind = entry.kind;
        event.name =
            entry.kind == AnimationKind::Transition ? entry.property : entry.keyframes.name;
        event.property = entry.property;
        event.elapsed = entry.timing.duration * entry.timing.iterations;
        pending_events_.push_back(std::move(event));
        if (entry.timing.fill == FillMode::Forwards || entry.timing.fill == FillMode::Both) {
            fills_.push_back(std::move(entry));
        }
        if (callback) {
            callbacks.push_back(FinishedCallback{id, std::move(callback)});
        }
    }
    for (FinishedCallback& entry : callbacks) {
        entry.callback(entry.id);
    }

    for (const AnimationEntry& entry : entries_) {
        AnimationSample sample = sample_entry(entry, entry.elapsed);
        result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
        result.samples.push_back(std::move(sample));
    }
    for (const AnimationEntry& fill : fills_) {
        AnimationSample sample = sample_entry(fill, fill.elapsed);
        result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
        result.samples.push_back(std::move(sample));
    }

    result.keep_awake = std::any_of(entries_.begin(), entries_.end(),
                                    [this](const AnimationEntry& entry) { return !frozen(entry); });
    return result;
}

std::size_t AnimationRuntime::non_essential_active() const {
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const AnimationEntry& entry) {
            return entry.non_essential && !entry.paused;
        }));
}

std::size_t AnimationRuntime::suspended() const {
    if (!suspend_non_essential_) {
        return 0;
    }
    return static_cast<std::size_t>(
        std::count_if(entries_.begin(), entries_.end(), [](const AnimationEntry& entry) {
            return entry.non_essential && !entry.paused;
        }));
}

bool AnimationRuntime::pause(AnimationId id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const AnimationEntry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        return false;
    }
    found->paused = true;
    return true;
}

bool AnimationRuntime::resume(AnimationId id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const AnimationEntry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        return false;
    }
    found->paused = false;
    return true;
}

std::size_t AnimationRuntime::pause_target(std::uint64_t target) {
    std::size_t count = 0;
    for (AnimationEntry& entry : entries_) {
        if (entry.target == target) {
            entry.paused = true;
            ++count;
        }
    }
    return count;
}

std::size_t AnimationRuntime::pause_target_name(std::uint64_t target, std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    std::size_t count = 0;
    for (AnimationEntry& entry : entries_) {
        if (entry.target == target && entry.keyframes.name == name) {
            entry.paused = true;
            ++count;
        }
    }
    return count;
}

std::size_t AnimationRuntime::resume_target(std::uint64_t target) {
    std::size_t count = 0;
    for (AnimationEntry& entry : entries_) {
        if (entry.target == target) {
            entry.paused = false;
            ++count;
        }
    }
    return count;
}

std::size_t AnimationRuntime::resume_target_name(std::uint64_t target, std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    std::size_t count = 0;
    for (AnimationEntry& entry : entries_) {
        if (entry.target == target && entry.keyframes.name == name) {
            entry.paused = false;
            ++count;
        }
    }
    return count;
}

std::size_t AnimationRuntime::stop_target(std::uint64_t target) {
    const auto removed =
        std::remove_if(entries_.begin(), entries_.end(),
                       [&](const AnimationEntry& entry) { return entry.target == target; });
    const std::size_t count = static_cast<std::size_t>(entries_.end() - removed);
    entries_.erase(removed, entries_.end());

    const auto removed_fills =
        std::remove_if(fills_.begin(), fills_.end(),
                       [&](const AnimationEntry& entry) { return entry.target == target; });
    const std::size_t fill_count = static_cast<std::size_t>(fills_.end() - removed_fills);
    fills_.erase(removed_fills, fills_.end());
    return count + fill_count;
}

std::size_t AnimationRuntime::stop_target_name(std::uint64_t target, std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    const auto removed =
        std::remove_if(entries_.begin(), entries_.end(), [&](const AnimationEntry& entry) {
            return entry.target == target && entry.keyframes.name == name;
        });
    const std::size_t count = static_cast<std::size_t>(entries_.end() - removed);
    entries_.erase(removed, entries_.end());

    const auto removed_fills =
        std::remove_if(fills_.begin(), fills_.end(), [&](const AnimationEntry& entry) {
            return entry.target == target && entry.keyframes.name == name;
        });
    const std::size_t fill_count = static_cast<std::size_t>(fills_.end() - removed_fills);
    fills_.erase(removed_fills, fills_.end());
    return count + fill_count;
}

bool AnimationRuntime::restart_target_name(std::uint64_t target, std::string_view name) {
    if (name.empty()) {
        return false;
    }
    bool found = false;
    for (AnimationEntry& entry : entries_) {
        if (entry.target == target && entry.keyframes.name == name) {
            entry.elapsed = 0.0;
            entry.paused = false;
            found = true;
        }
    }

    std::vector<AnimationEntry> revived;
    const auto removed = std::remove_if(fills_.begin(), fills_.end(), [&](AnimationEntry& entry) {
        if (entry.target != target || entry.keyframes.name != name) {
            return false;
        }
        entry.elapsed = 0.0;
        entry.paused = false;
        revived.push_back(std::move(entry));
        return true;
    });
    fills_.erase(removed, fills_.end());
    for (AnimationEntry& entry : revived) {
        entries_.push_back(std::move(entry));
    }
    return found || !revived.empty();
}

bool AnimationRuntime::restart(AnimationId id) {
    if (id == 0) {
        return false;
    }
    const auto active = std::find_if(entries_.begin(), entries_.end(),
                                     [&](const AnimationEntry& entry) { return entry.id == id; });
    if (active != entries_.end()) {
        active->elapsed = 0.0;
        active->paused = false;
        return true;
    }

    const auto settled = std::find_if(fills_.begin(), fills_.end(),
                                      [&](const AnimationEntry& entry) { return entry.id == id; });
    if (settled == fills_.end()) {
        return false;
    }
    settled->elapsed = 0.0;
    settled->paused = false;
    entries_.push_back(std::move(*settled));
    fills_.erase(settled);
    return true;
}

bool AnimationRuntime::stop(AnimationId id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const AnimationEntry& entry) { return entry.id == id; });
    if (found != entries_.end()) {
        entries_.erase(found);
        return true;
    }
    const auto settled = std::find_if(fills_.begin(), fills_.end(),
                                      [&](const AnimationEntry& entry) { return entry.id == id; });
    if (settled == fills_.end()) {
        return false;
    }
    fills_.erase(settled);
    return true;
}

std::size_t AnimationRuntime::stop_target_property(std::uint64_t target,
                                                   std::string_view property) {
    if (property.empty()) {
        return 0;
    }
    const auto removed =
        std::remove_if(entries_.begin(), entries_.end(), [&](const AnimationEntry& entry) {
            return entry.target == target && entry.property == property;
        });
    const std::size_t count = static_cast<std::size_t>(entries_.end() - removed);
    entries_.erase(removed, entries_.end());

    const auto removed_fills =
        std::remove_if(fills_.begin(), fills_.end(), [&](const AnimationEntry& entry) {
            return entry.target == target && entry.property == property;
        });
    const std::size_t fill_count = static_cast<std::size_t>(fills_.end() - removed_fills);
    fills_.erase(removed_fills, fills_.end());
    return count + fill_count;
}

void AnimationRuntime::clear() {
    entries_.clear();
    fills_.clear();
}

AnimationListenerId AnimationRuntime::add_event_listener(AnimationEventListener listener) {
    if (!listener) {
        return 0;
    }
    const AnimationListenerId id = next_listener_id_++;
    listeners_.emplace_back(id, std::move(listener));
    return id;
}

bool AnimationRuntime::remove_event_listener(AnimationListenerId id) {
    const auto found = std::find_if(listeners_.begin(), listeners_.end(),
                                    [&](const auto& entry) { return entry.first == id; });
    if (found == listeners_.end()) {
        return false;
    }
    listeners_.erase(found);
    return true;
}

std::size_t AnimationRuntime::dispatch_events() {
    if (pending_events_.empty()) {
        return 0;
    }
    std::vector<AnimationEvent> events;
    events.swap(pending_events_);
    const auto snapshot = listeners_;
    for (const AnimationEvent& event : events) {
        for (const auto& [id, listener] : snapshot) {
            // A listener removed by an earlier listener in this dispatch must not run.
            const bool alive = std::any_of(listeners_.begin(), listeners_.end(),
                                           [&](const auto& entry) { return entry.first == id; });
            if (alive) {
                listener(event);
            }
        }
    }
    return events.size();
}

AnimationNameState AnimationRuntime::target_name_state(std::uint64_t target,
                                                       std::string_view name) const {
    AnimationNameState state;
    if (name.empty()) {
        return state;
    }
    for (const AnimationEntry& entry : entries_) {
        if (entry.target == target && entry.keyframes.name == name) {
            ++state.active;
        }
    }
    for (const AnimationEntry& entry : fills_) {
        if (entry.target == target && entry.keyframes.name == name) {
            ++state.settled;
        }
    }
    return state;
}

} // namespace tessera::ui::animation
