// SPDX-License-Identifier: MIT
//
// Minimal style-provider stub shared by the deterministic CPU scenes (vector and raster image).
// Not installed; test/POC scaffolding only.

#pragma once

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <map>
#include <utility>

namespace tessera::poc::batched2d {

class CpuSceneStyles final : public ui::paint::StyleProvider {
public:
    void set(ui::NodeId id, ui::style::ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ui::style::ComputedStyle{} : found->second;
    }

private:
    std::map<ui::NodeId, ui::style::ComputedStyle> styles_;
};

[[nodiscard]] inline ui::style::ComputedStyle
cpu_style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ui::style::ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

} // namespace tessera::poc::batched2d