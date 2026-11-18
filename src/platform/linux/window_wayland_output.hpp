// SPDX-License-Identifier: MIT
//
// The wl_output globals of the Wayland window backend (issue #504).
//
// The only fact this slice reads from an output is `wl_output.geometry.subpixel`, the panel's
// subpixel layout, which feeds `WindowMetrics::pixel_geometry` (display-aware-rendering.md section
// 2). It is the first wl_output binding, so it is also where a later scale/mode source belongs.
//
// Which output a window is on is the compositor's answer, not a guess: `wl_surface.enter` names the
// output the surface now overlaps and `wl_surface.leave` retracts it. Until the first `enter` (the
// surface has no buffer yet, so it overlaps nothing) the geometry is that of the only output when
// there is exactly one, and `Unknown` otherwise - never a stripe taken from an arbitrary monitor.
//
// Everything runs on the owning thread, from the dispatch that invokes the listeners, and holds no
// allocation: the table is a fixed array (a desktop with more outputs than `kMaxOutputs` ignores
// the extras, which only costs them an `Unknown` answer).

#pragma once

#include "tessera/hal/display.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

struct wl_output;
struct wl_registry;
struct wl_surface;

namespace tessera::hal::detail {

class WaylandOutputs final {
public:
    WaylandOutputs() = default;
    ~WaylandOutputs();

    WaylandOutputs(const WaylandOutputs&) = delete;
    WaylandOutputs& operator=(const WaylandOutputs&) = delete;

    // Binds a `wl_output` global. Called from the registry listener with the global's `name`.
    void bind(wl_registry* registry, std::uint32_t name, std::uint32_t version);

    // The registry removed a global: drops the output when `name` was one.
    void remove(std::uint32_t name);

    // Starts listening for the surface's `enter`/`leave` so the window knows its output.
    void watch_surface(wl_surface* surface);

    // The subpixel layout of the output the window is on (see the file header).
    [[nodiscard]] PixelGeometry pixel_geometry() const noexcept;

    // True once after the answer `pixel_geometry()` would give changed: a late `geometry` event, a
    // move to another output, an output going away.
    [[nodiscard]] bool consume_change() noexcept;

    void destroy() noexcept;

    static constexpr std::size_t kMaxOutputs = 8;

private:
    friend struct WaylandOutputCallbacks;

    struct Slot {
        wl_output* output = nullptr;
        std::uint32_t name = 0;
        PixelGeometry geometry = PixelGeometry::Unknown;
    };

    void on_geometry(wl_output* output, std::uint32_t subpixel);
    void on_surface_enter(wl_output* output);
    void on_surface_leave(wl_output* output);
    [[nodiscard]] Slot* find(const wl_output* output) noexcept;
    void mark_changed_if_needed(PixelGeometry before) noexcept;

    std::array<Slot, kMaxOutputs> slots_{};
    const wl_output* current_ = nullptr;
    bool changed_ = false;
};

} // namespace tessera::hal::detail
