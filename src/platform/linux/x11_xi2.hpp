// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "x11_ime_composition.hpp"
#include "x11_xi2_translation.hpp"

#include <X11/Xlib.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(TESSERA_X11_XI2)
#include <X11/extensions/XInput2.h>
#endif

namespace tessera::hal {

// Xlib-facing XInput2 (XI2) pump: it owns the XI2 device table, selects the XI2 event masks on the
// window and translates device events through the pure rules in x11_xi2_translation.hpp. The type
// exists even when the build has no XInput2 headers (TESSERA_X11_XI2 undefined); initialize() then
// returns false and the window keeps the core-protocol path unchanged.
//
// What it selects and why (see docs/design/input.md section 2.2):
// - XI_Motion/XI_ButtonPress/XI_ButtonRelease on XIAllMasterDevices, so smooth-scroll valuators and
//   hardware wheel buttons are delivered as XI2 events. Per the XI2 protocol, delivering an event
//   as an XI event stops the core event for it, so the pump also produces the mouse/pen pointer
//   events the core path used to produce. Emulated duplicates (the server emulates wheel buttons
//   and valuators from each other, and pointer events from touch) carry XIPointerEmulated and are
//   dropped, which is what makes one physical notch produce exactly one event.
// - XI_TouchBegin/Update/End per touch-capable slave device (all three are required together).
// - XI_HierarchyChanged/XI_DeviceChanged on XIAllDevices to rebuild the device table; removing a
//   device cancels its live contacts.
class X11Xi2Pump {
public:
    void set_queue(ui::InputQueue* queue) {
        queue_ = queue;
    }

    // True while the queue holds input the application has not consumed yet (shared with the core
    // translator's queue, so either pump can wake the wait).
    [[nodiscard]] bool pending() const;

    // Queries the XInput extension (XI 2.2, or 2.1 for valuator scroll only), builds the device
    // table and selects the masks on `window`. Returns false without side effects when XI2 is not
    // available or the selection fails; the caller then keeps the core path.
    bool initialize(Display* display, ::Window window);
    void shutdown();
    [[nodiscard]] bool enabled() const {
        return enabled_;
    }
    // XInput extension major opcode (GenericEvent cookie `extension`), or -1.
    [[nodiscard]] int opcode() const {
        return opcode_;
    }

    // Handles one event whose cookie belongs to this extension. Returns true when the pump consumed
    // it. Fills the queue with normalized events.
    bool handle(XEvent& event, float scale);

    // Window focus loss/teardown: cancels every live touch contact and drops any half-accumulated
    // scroll remainder, so no pointer state survives an interaction reset.
    void cancel_all();

    // True when the input method can deliver client-side pre-edit (XIMPreeditCallbacks). The window
    // then creates the IC with that style and calls attach_input_context().
    [[nodiscard]] bool input_method_supports_preedit(XIM input_method) const;
    // Installs the pre-edit start/draw/done callbacks on an XIMPreeditCallbacks context. Returns
    // false when the method rejects them; the caller then recreates the IC without pre-edit.
    bool attach_input_context(XIM input_method, XIC context);

    // Called by the core key path for text committed by the input method (a KeyPress with keycode
    // 0): true when the pre-edit state machine already committed that exact text as an EditText
    // event, so the key event must be dropped instead of inserting the text twice.
    bool consume_commit(std::string_view text);

    // C callback thunk for the queue-side wiring (see X11InputTranslator::set_ime_commit_gate).
    static bool consume_commit_thunk(void* context, std::string_view text) {
        return static_cast<X11Xi2Pump*>(context)->consume_commit(text);
    }

private:
    void push(ui::InputEvent event);
    void track(float x, float y, float& dx, float& dy);

    bool rebuild_devices();
    bool select_events();
    [[nodiscard]] x11::DeviceProfile* find_device(std::uint32_t device_id);
    [[nodiscard]] const x11::DeviceProfile* find_device(std::uint32_t device_id) const;

    // IME pre-edit plumbing.
    void handle_preedit_start();
    void handle_preedit_draw(int caret, int change_first, int change_length, std::string_view text);
    void handle_preedit_done();
    void push_all(const x11::CompositionOutput& output);

#if defined(TESSERA_X11_XI2)
    bool handle_cookie(const XGenericEventCookie& cookie, float scale);
    void handle_motion(const XIDeviceEvent& device, x11::DeviceProfile& profile, float scale);
    void handle_button(const XIDeviceEvent& device, x11::DeviceProfile& profile, bool press,
                       float scale);
    void handle_touch(const XIDeviceEvent& device, int evtype, float scale);
    void handle_pen_event(const XIDeviceEvent& device, x11::DeviceProfile& profile,
                          ui::InputEventType type, ui::PointerButton button, bool in_contact,
                          float scale);
    void handle_hierarchy();
    [[nodiscard]] bool valuator_value(const XIValuatorState& state, int number, double& out) const;
    [[nodiscard]] bool wheel_from_valuators(const XIDeviceEvent& device,
                                            x11::DeviceProfile& profile, float x, float y,
                                            ui::Modifier modifiers);

    // XIM callback entry points (XIMProc). They reach the private handlers through the instance
    // stored in the callback's client_data.
    static void preedit_start_callback(XIM context, XPointer client_data, XPointer call_data);
    static void preedit_draw_callback(XIM context, XPointer client_data, XPointer call_data);
    static void preedit_done_callback(XIM context, XPointer client_data, XPointer call_data);
#endif

    Display* display_ = nullptr;
    ::Window window_ = 0;
    ui::InputQueue* queue_ = nullptr;
    bool enabled_ = false;
    bool has_touch_ = false;
    int opcode_ = -1;

    std::array<x11::DeviceProfile, x11::kMaxXi2Devices> devices_{};
    std::size_t device_count_ = 0;
    // Used for events from a device that is not in the table (it appeared after the table was
    // built, or the table is full): the event keeps the core-shape mouse behavior.
    x11::DeviceProfile fallback_{};

    // Standard tablet axis labels, interned once (None when the server never published them).
    Atom pressure_atom_ = 0;
    Atom pressure_atom_alt_ = 0;
    Atom tilt_x_atom_ = 0;
    Atom tilt_x_atom_alt_ = 0;
    Atom tilt_y_atom_ = 0;
    Atom tilt_y_atom_alt_ = 0;

    x11::TouchTracker touch_{};
    std::uint32_t buttons_down_ = 0;
    float last_x_ = 0.0F;
    float last_y_ = 0.0F;
    bool has_last_ = false;

    x11::CompositionState composition_{};
    // The XIM pre-edit is spliced here (bounded, UTF-8) before the state machine sees the whole
    // string, because XIMPreeditDraw sends incremental changes.
    std::array<char, x11::kMaxPreeditBytes> preedit_{};
    std::size_t preedit_size_ = 0;
    std::array<char, ui::kMaxEditTextBytes> committed_{};
    std::size_t committed_size_ = 0;
    bool commit_pending_ = false;
    bool preedit_attached_ = false;
};

} // namespace tessera::hal
