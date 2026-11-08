// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/animation/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

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
    AnimationEntry entry;
    entry.id = next_id_++;
    entry.target = input.target;
    entry.keyframes = std::move(input.keyframes);
    entry.timing = input.timing;
    entry.on_complete = std::move(input.on_complete);
    entry.property = std::move(input.property);
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

AnimationTickResult AnimationRuntime::tick(double dt) {
    AnimationTickResult result;
    if (!std::isfinite(dt) || dt < 0.0) {
        result.keep_awake = std::any_of(entries_.begin(), entries_.end(),
                                        [](const AnimationEntry& entry) { return !entry.paused; });
        return result;
    }

    std::vector<AnimationEntry> finished;
    for (AnimationEntry& entry : entries_) {
        if (entry.paused) {
            AnimationSample sample = sample_entry(entry, entry.elapsed);
            result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
            result.samples.push_back(std::move(sample));
            continue;
        }

        entry.elapsed += dt;
        if (!is_finished(entry)) {
            AnimationSample sample = sample_entry(entry, entry.elapsed);
            result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
            result.samples.push_back(std::move(sample));
            continue;
        }

        result.finished.push_back(entry.id);
        if (entry.timing.fill == FillMode::Forwards || entry.timing.fill == FillMode::Both) {
            const double total =
                entry.timing.delay + entry.timing.duration * entry.timing.iterations;
            AnimationSample sample = sample_entry(entry, total);
            result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
            result.samples.push_back(std::move(sample));
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

    for (AnimationEntry& entry : finished) {
        if (entry.on_complete) {
            entry.on_complete(entry.id);
        }
    }

    result.keep_awake = std::any_of(entries_.begin(), entries_.end(),
                                    [](const AnimationEntry& entry) { return !entry.paused; });
    return result;
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

bool AnimationRuntime::stop(AnimationId id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const AnimationEntry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
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
    return count;
}

void AnimationRuntime::clear() {
    entries_.clear();
}

} // namespace tessera::ui::animation
