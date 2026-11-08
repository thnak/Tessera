// SPDX-License-Identifier: MIT

#include "tessera/ui/animation/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::animation {
namespace {

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_layout_property(std::string_view property) {
    if (property == "display" || property == "position" || property == "box-sizing" ||
        property == "width" || property == "height" || property == "min-width" ||
        property == "min-height" || property == "max-width" || property == "max-height" ||
        property == "aspect-ratio" || property == "gap" || property == "row-gap" ||
        property == "column-gap" || property == "font-family" || property == "font-size" ||
        property == "font-weight" || property == "font-style" || property == "line-height" ||
        property == "letter-spacing" || property == "word-spacing" || property == "white-space" ||
        property == "text-align" || property == "text-transform" || property == "writing-mode") {
        return true;
    }
    return starts_with(property, "margin-") || starts_with(property, "padding-") ||
           starts_with(property, "border-") || starts_with(property, "flex-") ||
           starts_with(property, "grid-") || starts_with(property, "align-") ||
           starts_with(property, "justify-") || property == "top" || property == "right" ||
           property == "bottom" || property == "left";
}

Dirty dirty_for(const std::vector<style::Declaration>& declarations) {
    Dirty dirty = Dirty::None;
    for (const style::Declaration& declaration : declarations) {
        if (is_layout_property(declaration.property)) {
            dirty = dirty | Dirty::Layout | Dirty::Paint | Dirty::Render;
        } else {
            dirty = dirty | Dirty::Paint | Dirty::Render;
        }
    }
    return dirty;
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
    AnimationEntry entry;
    entry.id = next_id_++;
    entry.target = input.target;
    entry.keyframes = std::move(input.keyframes);
    entry.timing = input.timing;
    entry.on_complete = std::move(input.on_complete);
    entries_.push_back(std::move(entry));
    return entries_.back().id;
}

AnimationTickResult AnimationRuntime::tick(double dt) {
    AnimationTickResult result;
    if (!std::isfinite(dt) || dt < 0.0) {
        result.keep_awake = !entries_.empty();
        return result;
    }

    std::vector<AnimationEntry> finished;
    for (AnimationEntry& entry : entries_) {
        entry.elapsed += dt;
        if (!is_finished(entry)) {
            AnimationSample sample = sample_entry(entry, entry.elapsed);
            result.dirty = result.dirty | dirty_for(sample.declarations);
            result.samples.push_back(std::move(sample));
            continue;
        }

        result.finished.push_back(entry.id);
        if (entry.timing.fill == FillMode::Forwards || entry.timing.fill == FillMode::Both) {
            const double total =
                entry.timing.delay + entry.timing.duration * entry.timing.iterations;
            AnimationSample sample = sample_entry(entry, total);
            result.dirty = result.dirty | dirty_for(sample.declarations);
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

    result.keep_awake = !entries_.empty();
    return result;
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

void AnimationRuntime::clear() {
    entries_.clear();
}

} // namespace tessera::ui::animation
