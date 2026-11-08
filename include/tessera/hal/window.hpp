// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace tessera::hal {

// Logical-pixel insets owned by the platform (safe area, system bars, notch, IME).
struct WindowInsets {
    float left = 0;
    float top = 0;
    float right = 0;
    float bottom = 0;
};

// Immutable-per-frame window/display snapshot. Styles, layout, and input see this type and never
// an OS handle, OS message, or swapchain.
struct WindowMetrics {
    std::uint32_t logical_width = 0;
    std::uint32_t logical_height = 0;
    std::uint32_t physical_width = 0;
    std::uint32_t physical_height = 0;
    float dpi_scale = 1.0f;
    WindowInsets insets;
    bool focused = true;
    bool occluded = false;
    bool minimized = false;
};

enum class WindowEventType {
    Resize,
    DpiChange,
    Focus,
    Blur,
    Occluded,
    Minimized,
    Restored,
    Close,
};

struct WindowEvent {
    WindowEventType type = WindowEventType::Resize;
    WindowMetrics metrics;
};

// Client-area size is logical pixels; the platform converts it to physical pixels with the
// per-monitor DPI scale. A zero width or height is invalid.
struct WindowDesc {
    std::string title = "Tessera";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    bool visible = true;
};

// Platform window seam. Implementations live under src/platform/<os>/ and own every OS handle;
// this header stays OS-free so UI and rendering code can depend on it.
class Window {
public:
    virtual ~Window() = default;

    [[nodiscard]] virtual WindowMetrics metrics() const = 0;

    // Copies pending events oldest first into `events` and returns the count written. Events that
    // do not fit stay queued for the next poll. Never blocks and never calls user code.
    virtual std::size_t poll_events(std::span<WindowEvent> events) = 0;

    virtual void show() = 0;
    virtual void hide() = 0;

    // Asks the runtime for a frame; coalesced, so repeated calls before a frame are harmless.
    virtual void request_redraw() = 0;

    // Requests graceful shutdown. Implementations emit WindowEventType::Close; calling twice is
    // harmless.
    virtual void close() = 0;
};

// Creates a platform window from `desc`. Platforms without a window backend return nullptr;
// invalid descriptors (zero size) also return nullptr. The returned window must be destroyed on
// the thread that created it.
std::unique_ptr<Window> create_window(const WindowDesc& desc);

} // namespace tessera::hal
