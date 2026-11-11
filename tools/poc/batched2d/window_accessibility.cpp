// SPDX-License-Identifier: MIT
//
// Windowed-POC accessibility wiring: the real window answers WM_GETOBJECT through the UIA host and
// reports its HWND as the root's host element. Only the platform/UIA layer ever interprets the
// opaque handle; this file just passes it through.

#include "window_accessibility.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>

namespace tessera::poc::batched2d {
namespace {

void set_text(std::array<char, ui::a11y::kMaxTextBytes>& out, std::string_view text) {
    const std::size_t count = std::min(text.size(), out.size() - 1);
    std::memcpy(out.data(), text.data(), count);
    out[count] = '\0';
}

} // namespace

WindowAccessibility::WindowAccessibility(hal::Window& window)
    : window_(&window) {
    source_ = [this](ui::a11y::A11ySnapshot& snapshot) { build_tree(*window_, snapshot); };
    auto host = std::make_unique<ui::a11y::UiaHost>();
    // Order matters only for readability: the handle may be installed before the bridge starts, and
    // the root answer is one atomic load that stays "not ready" until the bridge publishes.
    host->set_host_handle(window.accessibility_host_handle());
    window.set_accessibility_root(ui::a11y::make_window_root_provider(*host));
    if (!host->start().has_value()) {
        // Accessibility off (a build without a UIA platform): restore the default provider.
        window.set_accessibility_root({});
        return;
    }
    host->mark_dirty();
    host_ = std::move(host);
}

WindowAccessibility::~WindowAccessibility() {
    if (window_ != nullptr) {
        window_->set_accessibility_root({});
    }
    // Stops the bridge and releases the root while the window (and its HWND) is still valid.
    host_.reset();
}

void WindowAccessibility::tick() {
    if (host_ == nullptr) {
        return;
    }
    const hal::WindowMetrics metrics = window_->metrics();
    if (metrics.logical_width != width_ || metrics.logical_height != height_) {
        width_ = metrics.logical_width;
        height_ = metrics.logical_height;
        host_->mark_dirty();
    }
    (void)host_->tick(source_);
}

void WindowAccessibility::build_tree(hal::Window& window, ui::a11y::A11ySnapshot& snapshot) {
    snapshot.clear();
    const hal::WindowMetrics metrics = window.metrics();
    ui::a11y::A11yNode root;
    root.id = 1;
    root.role = ui::a11y::Role::Generic;
    set_text(root.name, "Tessera windowed POC");
    set_text(root.description, "Batched 2D rendering demo window");
    root.bounds = {0.0F, 0.0F, static_cast<float>(metrics.logical_width),
                   static_cast<float>(metrics.logical_height)};
    snapshot.nodes.push_back(root);
}

} // namespace tessera::poc::batched2d
