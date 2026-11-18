// SPDX-License-Identifier: MIT
//
// wl_output tracking for the Wayland window backend (issue #504). See window_wayland_output.hpp.

#include "window_wayland_output.hpp"

#include <algorithm>
#include <cstdio>
#include <wayland-client.h>

namespace tessera::hal::detail {
namespace {

// wl_output 2 adds `scale` (an integer factor) and `done` (the end of an atomic property batch);
// version 3 only adds `release`, which this backend does not need, and version 4 `name` and
// `description`, which it does not read. Binding at 2 keeps those later events from arriving.
constexpr std::uint32_t kOutputVersion = 2;
constexpr std::uint32_t kModeCurrent = 0x1;
// wl_output.transform values that rotate by 90 or 270 degrees (with or without a flip) swap the
// mode's axes in the compositor's logical space.
constexpr bool transform_swaps_axes(std::uint32_t transform) noexcept {
    return (transform & 1u) != 0;
}

} // namespace

struct WaylandOutputCallbacks {
    static void geometry(void* data, wl_output* output, std::int32_t x, std::int32_t y,
                         std::int32_t, std::int32_t, std::int32_t subpixel, const char* make,
                         const char* model, std::int32_t transform) {
        static_cast<WaylandOutputs*>(data)->on_geometry(output,
                                                        {x, y, subpixel, transform, make, model});
    }
    static void mode(void* data, wl_output* output, std::uint32_t flags, std::int32_t width,
                     std::int32_t height, std::int32_t refresh) {
        static_cast<WaylandOutputs*>(data)->on_mode(output, flags, width, height, refresh);
    }
    static void done(void* data, wl_output* output) {
        static_cast<WaylandOutputs*>(data)->on_done(output);
    }
    static void scale(void* data, wl_output* output, std::int32_t factor) {
        static_cast<WaylandOutputs*>(data)->on_scale(output, factor);
    }
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
    *free_slot = Slot{};
    free_slot->output = output;
    free_slot->name = name;
    free_slot->version = std::min(version, kOutputVersion);
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
        const bool was_listed = slot.ready;
        slot = Slot{};
        mark_changed_if_needed(before);
        changed_ = changed_ || was_listed;
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

void WaylandOutputs::settle(Slot& slot) {
    slot.pending = false;
    changed_ = true;
}

// Version 2 batches property events and ends them with `done`; a version 1 output has no `done`, so
// each event is its own batch.
void WaylandOutputs::on_geometry(wl_output* output, const GeometryEvent& event) {
    Slot* slot = find(output);
    if (slot == nullptr) {
        return;
    }
    slot->ready = true;
    slot->x = event.x;
    slot->y = event.y;
    // The enum is unsigned on the wire; a negative value is garbage and reads as unknown.
    slot->geometry = wayland_subpixel_geometry(
        event.subpixel < 0 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(event.subpixel));
    slot->transform = event.transform < 0 ? 0u : static_cast<std::uint32_t>(event.transform);
    const char* make = event.make != nullptr ? event.make : "";
    const char* model = event.model != nullptr ? event.model : "";
    if (make[0] != '\0' || model[0] != '\0') {
        std::snprintf(slot->label.data(), slot->label.size(), "%s %s", make, model);
    } else {
        std::snprintf(slot->label.data(), slot->label.size(), "wl_output-%u", slot->name);
    }
    slot->pending = true;
    if (slot->version < 2) {
        settle(*slot);
    }
}

void WaylandOutputs::on_mode(wl_output* output, std::uint32_t flags, std::int32_t width,
                             std::int32_t height, std::int32_t refresh_mhz) {
    Slot* slot = find(output);
    if (slot == nullptr || (flags & kModeCurrent) == 0) {
        return;
    }
    slot->mode_width = width;
    slot->mode_height = height;
    slot->refresh_mhz = refresh_mhz;
    slot->pending = true;
    if (slot->version < 2) {
        settle(*slot);
    }
}

void WaylandOutputs::on_scale(wl_output* output, std::int32_t factor) {
    Slot* slot = find(output);
    if (slot == nullptr) {
        return;
    }
    slot->scale = factor > 0 ? factor : 1;
    slot->scale_seen = factor > 0;
    slot->pending = true;
}

void WaylandOutputs::on_done(wl_output* output) {
    Slot* slot = find(output);
    if (slot != nullptr && slot->pending) {
        settle(*slot);
    }
}

std::size_t WaylandOutputs::displays(std::span<DisplayInfo> out) const noexcept {
    std::size_t written = 0;
    for (const Slot& slot : slots_) {
        if (slot.output == nullptr || !slot.ready || written >= out.size()) {
            continue;
        }
        DisplayInfo& info = out[written];
        info = DisplayInfo{};
        info.id = slot.name;
        info.name = slot.label;
        info.name.back() = '\0';
        const bool swap = transform_swaps_axes(slot.transform);
        const std::int32_t width = std::max(swap ? slot.mode_height : slot.mode_width, 0);
        const std::int32_t height = std::max(swap ? slot.mode_width : slot.mode_height, 0);
        info.physical_width = static_cast<std::uint32_t>(width);
        info.physical_height = static_cast<std::uint32_t>(height);
        info.scale = static_cast<float>(slot.scale);
        info.scale_assumed = !slot.scale_seen;
        info.bounds = WindowRect{static_cast<float>(slot.x), static_cast<float>(slot.y),
                                 static_cast<float>(width / slot.scale),
                                 static_cast<float>(height / slot.scale)};
        // The protocol's refresh is millihertz; 0 means the compositor did not say.
        if (slot.refresh_mhz > 0) {
            info.refresh_hz = static_cast<float>(slot.refresh_mhz) / 1000.0f;
            info.refresh_hz_assumed = false;
        } else {
            info.refresh_hz_assumed = true;
        }
        info.pixel_geometry = slot.geometry;
        ++written;
    }
    return written;
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
