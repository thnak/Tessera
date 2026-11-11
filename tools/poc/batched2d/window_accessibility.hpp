// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/accessibility/uia_host.hpp"

#include <cstdint>
#include <memory>

namespace tessera::poc::batched2d {

// Windowed-POC accessibility root: installs the WM_GETOBJECT answer plus the window's opaque native
// host handle, and publishes one static "demo window" pane so a screen reader can find the real
// window. Non-Windows builds and a failed start leave the window on its default provider; `tick`
// then does nothing at all.
class WindowAccessibility {
public:
    explicit WindowAccessibility(hal::Window& window);
    ~WindowAccessibility();

    WindowAccessibility(const WindowAccessibility&) = delete;
    WindowAccessibility& operator=(const WindowAccessibility&) = delete;

    [[nodiscard]] bool enabled() const {
        return host_ != nullptr;
    }

    // UI thread, once per loop iteration: rebuilds only on a size change or the first tick, then
    // publishes only when the host is dirty. A static window does no UIA work.
    void tick();

private:
    static void build_tree(hal::Window& window, ui::a11y::A11ySnapshot& snapshot);

    hal::Window* window_ = nullptr;
    std::unique_ptr<ui::a11y::UiaHost> host_;
    // Cached so a tick never allocates a std::function after the first.
    ui::a11y::UiaHost::SnapshotSource source_;
    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
};

} // namespace tessera::poc::batched2d
