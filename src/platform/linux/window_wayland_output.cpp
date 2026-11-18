// SPDX-License-Identifier: MIT
//
// wl_output tracking for the Wayland window backend (issue #504). See window_wayland_output.hpp.

#include "window_wayland_output.hpp"

#include <algorithm>
#include <wayland-client.h>

namespace tessera::hal::detail {
namespace {

// wl_output 2 adds `scale` and `done`, neither of which this slice reads. Version 1 carries the
// `geometry` event with the subpixel enum, which is all that is needed; binding no higher also
// means the version 4 `name`/`description` events can never arrive.
constexpr std::uint32_t kOutputVersion = 2;

} // namespace

struct WaylandOutputCallbacks {
    static void geometry(void* data, wl_output* output, std::int32_t, std::int32_t, std::int32_t,
                         std::int32_t, std::int32_t subpixel, const char*, const char*,
                         std::int32_t) {
        // The enum is unsigned on the wire; a negative value is garbage and reads as unknown.
        static_cast<WaylandOutputs*>(data)->on_geometry(
            output, subpixel < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(subpixel));
    }
    static void mode(void*, wl_output*, std::uint32_t, std::int32_t, std::int32_t, std::int32_t) {}
    static void done(void*, wl_output*) {}
    static void scale(void*, wl_output*, std::int32_t) {}
    static void name(void*, wl_output*, const char*) {}
    static void description(void*, wl_output*, const char*) {}

    static void enter(void* data, wl_surface*, wl_output* output) {
        static_cast<WaylandOutputs*>(data)->on_surface_enter(output);
    }
    static void leave(void* data, wl_surface*, wl_output* output) {
        static_cast<WaylandOutputs*>(data)->on_surface_leave(output);
    }
    static void preferred_buffer_scale(void*, wl_surface*, std::int32_t) {}
    static void preferred_buffer_transform(void*, wl_surface*, std::uint32_t) {}
};

namespace {

// Value-initialised then assigned, so a libwayland whose listener struct has more or fewer members
// than this one still builds; every member the build knows is a no-op or a real handler.
wl_output_listener make_output_listener() {
    wl_output_listener listener{};
    listener.geometry = WaylandOutputCallbacks::geometry;
    listener.mode = WaylandOutputCallbacks::mode;
    listener.done = WaylandOutputCallbacks::done;
    listener.scale = WaylandOutputCallbacks::scale;
    listener.name = WaylandOutputCallbacks::name;
    listener.description = WaylandOutputCallbacks::description;
    return listener;
}

wl_surface_listener make_surface_listener() {
    wl_surface_listener listener{};
    listener.enter = WaylandOutputCallbacks::enter;
    listener.leave = WaylandOutputCallbacks::leave;
    listener.preferred_buffer_scale = WaylandOutputCallbacks::preferred_buffer_scale;
    listener.preferred_buffer_transform = WaylandOutputCallbacks::preferred_buffer_transform;
    return listener;
}

const wl_output_listener kOutputListener = make_output_listener();
const wl_surface_listener kSurfaceListener = make_surface_listener();

} // namespace

WaylandOutputs::~WaylandOutputs() {
    destroy();
}

WaylandOutputs::Slot* WaylandOutputs::find(const wl_output* output) noexcept {
    for (Slot& slot : slots_) {
        if (slot.output != nullptr && slot.output == output) {
            return &slot;
        }
    }
    return nullptr;
}

void WaylandOutputs::bind(wl_registry* registry, std::uint32_t name, std::uint32_t version) {
    Slot* free_slot = nullptr;
    for (Slot& slot : slots_) {
        if (slot.output == nullptr) {
            free_slot = &slot;
            break;
        }
    }
    if (free_slot == nullptr || registry == nullptr) {
        return;
    }
    const PixelGeometry before = pixel_geometry();
    auto* output = static_cast<wl_output*>(
        wl_registry_bind(registry, name, &wl_output_interface, std::min(version, kOutputVersion)));
    if (output == nullptr) {
        return;
    }
    free_slot->output = output;
    free_slot->name = name;
    free_slot->geometry = PixelGeometry::Unknown;
    wl_output_add_listener(output, &kOutputListener, this);
    mark_changed_if_needed(before);
}

void WaylandOutputs::remove(std::uint32_t name) {
    for (Slot& slot : slots_) {
        if (slot.output == nullptr || slot.name != name) {
            continue;
        }
        const PixelGeometry before = pixel_geometry();
        if (current_ == slot.output) {
            current_ = nullptr;
        }
        // Bound below version 3 there is no `release` request, so only the proxy is freed.
        wl_output_destroy(slot.output);
        slot = Slot{};
        mark_changed_if_needed(before);
        return;
    }
}

void WaylandOutputs::watch_surface(wl_surface* surface) {
    if (surface != nullptr) {
        wl_surface_add_listener(surface, &kSurfaceListener, this);
    }
}

PixelGeometry WaylandOutputs::pixel_geometry() const noexcept {
    const Slot* sole = nullptr;
    std::size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.output == nullptr) {
            continue;
        }
        if (slot.output == current_) {
            return slot.geometry;
        }
        sole = &slot;
        ++count;
    }
    return count == 1 ? sole->geometry : PixelGeometry::Unknown;
}

bool WaylandOutputs::consume_change() noexcept {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

void WaylandOutputs::mark_changed_if_needed(PixelGeometry before) noexcept {
    if (pixel_geometry() != before) {
        changed_ = true;
    }
}

void WaylandOutputs::on_geometry(wl_output* output, std::uint32_t subpixel) {
    Slot* slot = find(output);
    if (slot == nullptr) {
        return;
    }
    const PixelGeometry before = pixel_geometry();
    slot->geometry = wayland_subpixel_geometry(subpixel);
    mark_changed_if_needed(before);
}

void WaylandOutputs::on_surface_enter(wl_output* output) {
    // `enter` can name an output this backend did not bind (past `kMaxOutputs`); the answer then
    // falls back to the sole-output rule instead of pointing at a dead slot.
    const PixelGeometry before = pixel_geometry();
    current_ = find(output) != nullptr ? output : nullptr;
    mark_changed_if_needed(before);
}

void WaylandOutputs::on_surface_leave(wl_output* output) {
    if (current_ != output) {
        return;
    }
    const PixelGeometry before = pixel_geometry();
    current_ = nullptr;
    mark_changed_if_needed(before);
}

void WaylandOutputs::destroy() noexcept {
    for (Slot& slot : slots_) {
        if (slot.output != nullptr) {
            wl_output_destroy(slot.output);
            slot = Slot{};
        }
    }
    current_ = nullptr;
    changed_ = false;
}

} // namespace tessera::hal::detail
