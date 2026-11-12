// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "x11_input_translation.hpp"
#include "x11_xi2.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace tessera::hal {
namespace {

float sanitize_scale(float scale) {
    return scale > 0.0F ? scale : 1.0F;
}

#if defined(TESSERA_X11_XI2)
// Xlib's default error handler terminates the process. XI2 setup requests can legitimately be
// refused (an old server rejecting a touch mask, a device that vanished between queries), so the
// requests are issued under a scoped trap that records the protocol error instead.
int g_trapped_error = 0;

int trap_error(Display* /*display*/, XErrorEvent* event) {
    g_trapped_error = event != nullptr ? event->error_code : 0;
    return 0;
}

class ScopedErrorTrap {
public:
    explicit ScopedErrorTrap(Display* display)
        : display_(display),
          previous_(XSetErrorHandler(trap_error)) {
        g_trapped_error = 0;
    }
    ~ScopedErrorTrap() {
        XSetErrorHandler(previous_);
    }

    ScopedErrorTrap(const ScopedErrorTrap&) = delete;
    ScopedErrorTrap& operator=(const ScopedErrorTrap&) = delete;

    // Protocol errors arrive asynchronously, so the round trip is forced before the answer is read.
    [[nodiscard]] bool failed() {
        XSync(display_, False);
        return g_trapped_error != 0;
    }

private:
    Display* display_ = nullptr;
    int (*previous_)(Display*, XErrorEvent*) = nullptr;
};

bool label_is(Atom label, Atom first, Atom second) {
    return label != 0 && (label == first || label == second);
}

#endif

} // namespace
bool X11Xi2Pump::pending() const {
    return queue_ != nullptr && !queue_->empty();
}

void X11Xi2Pump::push(ui::InputEvent event) {
    if (queue_ == nullptr) {
        return;
    }
    event.timestamp_s = steady_now_seconds();
    // A full queue sets its overflow flag; the application reports it.
    (void)queue_->push(event);
}

void X11Xi2Pump::track(float x, float y, float& dx, float& dy) {
    dx = has_last_ ? x - last_x_ : 0.0F;
    dy = has_last_ ? y - last_y_ : 0.0F;
    last_x_ = x;
    last_y_ = y;
    has_last_ = true;
}

void X11Xi2Pump::push_all(const x11::CompositionOutput& output) {
    for (std::size_t index = 0; index < output.count; ++index) {
        push(output.events[index]);
    }
}

bool X11Xi2Pump::initialize(Display* display, ::Window window) {
    if (display == nullptr || window == 0) {
        return false;
    }
    display_ = display;
    window_ = window;
#if !defined(TESSERA_X11_XI2)
    return false;
#else
    int opcode = 0;
    int event_base = 0;
    int error_base = 0;
    if (XQueryExtension(display_, "XInputExtension", &opcode, &event_base, &error_base) == 0) {
        return false;
    }
    // XI 2.2 adds touch; 2.1 already has scroll valuators. Ask for 2.2 and retry at 2.1 so an older
    // server keeps smooth scrolling instead of falling back to the core notches.
    int major = 2;
    int minor = 2;
    int version = XIQueryVersion(display_, &major, &minor);
    bool touch_supported = version == Success && major == 2 && minor >= 2;
    if (version != Success) {
        major = 2;
        minor = 1;
        version = XIQueryVersion(display_, &major, &minor);
        touch_supported = false;
    }
    if (version != Success || major < 2) {
        return false;
    }
    opcode_ = opcode;
    has_touch_ = touch_supported;
    // Standard tablet axis labels (the xf86 drivers publish both spellings).
    pressure_atom_ = XInternAtom(display_, "Abs Pressure", True);
    pressure_atom_alt_ = XInternAtom(display_, "Pressure", True);
    tilt_x_atom_ = XInternAtom(display_, "Abs Tilt X", True);
    tilt_x_atom_alt_ = XInternAtom(display_, "Tilt X", True);
    tilt_y_atom_ = XInternAtom(display_, "Abs Tilt Y", True);
    tilt_y_atom_alt_ = XInternAtom(display_, "Tilt Y", True);
    if (!rebuild_devices() || !select_events()) {
        shutdown();
        return false;
    }
    enabled_ = true;
    return true;
#endif
}

void X11Xi2Pump::shutdown() {
    // No device grabs are taken: the pump only selects events, and the server clears the selection
    // when the window or the connection goes away.
    enabled_ = false;
    has_touch_ = false;
    opcode_ = -1;
    display_ = nullptr;
    window_ = 0;
    device_count_ = 0;
    devices_ = {};
    touch_.reset();
    composition_.reset();
    preedit_size_ = 0;
    committed_size_ = 0;
    commit_pending_ = false;
    preedit_attached_ = false;
    buttons_down_ = 0;
    has_last_ = false;
}

x11::DeviceProfile* X11Xi2Pump::find_device(std::uint32_t device_id) {
    for (std::size_t index = 0; index < device_count_; ++index) {
        if (devices_[index].device_id == device_id) {
            return &devices_[index];
        }
    }
    return nullptr;
}

const x11::DeviceProfile* X11Xi2Pump::find_device(std::uint32_t device_id) const {
    for (std::size_t index = 0; index < device_count_; ++index) {
        if (devices_[index].device_id == device_id) {
            return &devices_[index];
        }
    }
    return nullptr;
}

bool X11Xi2Pump::rebuild_devices() {
#if !defined(TESSERA_X11_XI2)
    return false;
#else
    devices_ = {};
    device_count_ = 0;
    int count = 0;
    XIDeviceInfo* info = XIQueryDevice(display_, XIAllDevices, &count);
    if (info == nullptr) {
        return false;
    }
    for (int index = 0; index < count && device_count_ < x11::kMaxXi2Devices; ++index) {
        const XIDeviceInfo& device = info[index];
        // Only slave pointers carry the physical device's classes; master devices aggregate them
        // and a floating slave (a tablet with no cursor) still delivers device events.
        if (device.use != XISlavePointer && device.use != XIFloatingSlave) {
            continue;
        }
        x11::DeviceProfile profile;
        profile.device_id = static_cast<std::uint32_t>(device.deviceid);
        profile.tool = x11::pen_tool_from_device_name(
            device.name != nullptr ? std::string_view(device.name) : std::string_view{});
        float vertical_increment = 0.0F;
        float horizontal_increment = 0.0F;
        for (int class_index = 0; class_index < device.num_classes; ++class_index) {
            const XIAnyClassInfo* any = device.classes[class_index];
            if (any == nullptr) {
                continue;
            }
            if (any->type == XIValuatorClass) {
                const auto* valuator = reinterpret_cast<const XIValuatorClassInfo*>(any);
                if (label_is(valuator->label, pressure_atom_, pressure_atom_alt_)) {
                    profile.pressure_valuator = valuator->number;
                    profile.pressure_min = static_cast<float>(valuator->min);
                    profile.pressure_max = static_cast<float>(valuator->max);
                } else if (label_is(valuator->label, tilt_x_atom_, tilt_x_atom_alt_)) {
                    profile.tilt_x_valuator = valuator->number;
                } else if (label_is(valuator->label, tilt_y_atom_, tilt_y_atom_alt_)) {
                    profile.tilt_y_valuator = valuator->number;
                }
            } else if (any->type == XIScrollClass) {
                const auto* scroll = reinterpret_cast<const XIScrollClassInfo*>(any);
                if (scroll->scroll_type == XIScrollTypeVertical) {
                    profile.vertical_valuator = scroll->number;
                    vertical_increment = static_cast<float>(scroll->increment);
                    profile.scroll_capable = true;
                } else if (scroll->scroll_type == XIScrollTypeHorizontal) {
                    profile.horizontal_valuator = scroll->number;
                    horizontal_increment = static_cast<float>(scroll->increment);
                    profile.scroll_capable = true;
                }
            } else if (any->type == XITouchClass) {
                profile.touch_capable = true;
            }
        }
        profile.scroll.set_vertical_increment(vertical_increment);
        profile.scroll.set_horizontal_increment(horizontal_increment);
        devices_[device_count_] = profile;
        ++device_count_;
    }
    XIFreeDeviceInfo(info);
    return true;
#endif
}

bool X11Xi2Pump::select_events() {
#if !defined(TESSERA_X11_XI2)
    return false;
#else
    // Pointer events from every master device: delivering them as XI2 events is what stops the
    // server from also sending the core event for the same motion/button.
    unsigned char pointer_mask[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(pointer_mask, XI_Motion);
    XISetMask(pointer_mask, XI_ButtonPress);
    XISetMask(pointer_mask, XI_ButtonRelease);
    XIEventMask pointer_selection{};
    pointer_selection.deviceid = XIAllMasterDevices;
    pointer_selection.mask_len = static_cast<int>(sizeof(pointer_mask));
    pointer_selection.mask = pointer_mask;

    // The hierarchy mask may only be selected for XIAllDevices.
    unsigned char hierarchy_mask[XIMaskLen(XI_LASTEVENT)] = {};
    XISetMask(hierarchy_mask, XI_HierarchyChanged);
    XIEventMask hierarchy_selection{};
    hierarchy_selection.deviceid = XIAllDevices;
    hierarchy_selection.mask_len = static_cast<int>(sizeof(hierarchy_mask));
    hierarchy_selection.mask = hierarchy_mask;

    ScopedErrorTrap trap(display_);
    XISelectEvents(display_, window_, &pointer_selection, 1);
    XISelectEvents(display_, window_, &hierarchy_selection, 1);
    if (has_touch_) {
        // Touch has to be selected per device, and all three phases plus the ownership mask are
        // required together; a device that is not touch-capable rejects the mask.
        unsigned char touch_mask[XIMaskLen(XI_LASTEVENT)] = {};
        XISetMask(touch_mask, XI_TouchBegin);
        XISetMask(touch_mask, XI_TouchUpdate);
        XISetMask(touch_mask, XI_TouchEnd);
        for (std::size_t index = 0; index < device_count_; ++index) {
            if (!devices_[index].touch_capable) {
                continue;
            }
            XIEventMask touch_selection{};
            touch_selection.deviceid = static_cast<int>(devices_[index].device_id);
            touch_selection.mask_len = static_cast<int>(sizeof(touch_mask));
            touch_selection.mask = touch_mask;
            XISelectEvents(display_, window_, &touch_selection, 1);
        }
    }
    if (trap.failed()) {
        // Undo a partial selection: leaving the pointer mask selected while the pump is disabled
        // would suppress the core pointer events and leave the window without pointer input.
        XIEventMask clear_pointer{};
        clear_pointer.deviceid = XIAllMasterDevices;
        clear_pointer.mask_len = 0;
        clear_pointer.mask = nullptr;
        XISelectEvents(display_, window_, &clear_pointer, 1);
        XIEventMask clear_all{};
        clear_all.deviceid = XIAllDevices;
        clear_all.mask_len = 0;
        clear_all.mask = nullptr;
        XISelectEvents(display_, window_, &clear_all, 1);
        XSync(display_, False);
        return false;
    }
    return true;
#endif
}

bool X11Xi2Pump::handle(XEvent& event, float scale) {
#if !defined(TESSERA_X11_XI2)
    (void)event;
    (void)scale;
    return false;
#else
    if (!enabled_ || event.type != GenericEvent) {
        return false;
    }
    XGenericEventCookie* cookie = &event.xcookie;
    if (cookie->extension != opcode_) {
        return false;
    }
    if (XGetEventData(display_, cookie) == 0) {
        return false;
    }
    const bool handled = handle_cookie(*cookie, sanitize_scale(scale));
    XFreeEventData(display_, cookie);
    return handled;
#endif
}

void X11Xi2Pump::cancel_all() {
    std::array<ui::InputEvent, x11::kMaxTouchContacts> cancellations{};
    std::size_t written = 0;
    for (std::size_t index = 0; index < device_count_; ++index) {
        const std::size_t ended =
            touch_.cancel_device(devices_[index].device_id, cancellations.data() + written,
                                 cancellations.size() - written);
        written = std::min(written + ended, cancellations.size());
        if (written >= cancellations.size()) {
            break;
        }
    }
    for (std::size_t index = 0; index < written; ++index) {
        push(cancellations[index]);
    }
    touch_.reset();
    // Drop half-accumulated scroll and pointer state: after a focus change the next event must not
    // inherit a stale delta.
    for (std::size_t index = 0; index < device_count_; ++index) {
        devices_[index].scroll.reset();
    }
    buttons_down_ = 0;
    has_last_ = false;
    push_all(composition_.cancel());
}

} // namespace tessera::hal
