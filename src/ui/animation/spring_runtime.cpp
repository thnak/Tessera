// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/animation/spring_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

std::string format_value(float value) {
    if (std::abs(value) < 0.00005f) {
        value = 0.0f;
    }
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%.4f", value);
    std::string result(buffer);
    while (!result.empty() && result.back() == '0') {
        result.pop_back();
    }
    if (!result.empty() && result.back() == '.') {
        result.pop_back();
    }
    return result.empty() ? "0" : result;
}

} // namespace

AnimationId SpringRuntime::start(SpringAnimationInput input) {
    Entry entry;
    entry.id = next_id_++;
    entry.target = input.target;
    entry.property = std::move(input.property);
    entry.spring = Spring(input.config);
    entry.spring.snap_to(input.from);
    entry.spring.set_target(input.to);
    entry.on_complete = std::move(input.on_complete);
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

bool SpringRuntime::retarget(AnimationId id, float target) {
    if (!std::isfinite(target)) {
        return false;
    }
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        return false;
    }
    found->spring.set_target(target);
    return true;
}

AnimationTickResult SpringRuntime::tick(double dt) {
    AnimationTickResult result;
    if (!std::isfinite(dt) || dt < 0.0) {
        result.keep_awake = !entries_.empty();
        return result;
    }

    std::vector<Entry> finished;
    for (Entry& entry : entries_) {
        const bool active = entry.spring.tick(static_cast<float>(dt));

        AnimationSample sample;
        sample.id = entry.id;
        sample.target = entry.target;
        sample.declarations.push_back(
            style::Declaration{entry.property, format_value(entry.spring.position()), false, 0});
        result.dirty = result.dirty | dirty_for_declarations(sample.declarations);
        result.samples.push_back(std::move(sample));

        if (!active) {
            result.finished.push_back(entry.id);
            finished.push_back(std::move(entry));
        }
    }

    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& entry) {
                                      return std::any_of(
                                          finished.begin(), finished.end(),
                                          [&](const Entry& done) { return done.id == entry.id; });
                                  }),
                   entries_.end());

    for (Entry& entry : finished) {
        if (entry.on_complete) {
            entry.on_complete(entry.id);
        }
    }

    result.keep_awake = !entries_.empty();
    return result;
}

bool SpringRuntime::stop(AnimationId id) {
    const auto found = std::find_if(entries_.begin(), entries_.end(),
                                    [&](const Entry& entry) { return entry.id == id; });
    if (found == entries_.end()) {
        return false;
    }
    entries_.erase(found);
    return true;
}

void SpringRuntime::clear() {
    entries_.clear();
}

} // namespace tessera::ui::animation
