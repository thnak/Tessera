// SPDX-License-Identifier: MIT

#pragma once

#include "scene_session.hpp"
#include "scene_state.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/synthetic_events.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/ui/input.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// `platform.event` in the viewer (docs/specs/designer-protocol-v1.md section 5.1, issue #446;
// docs/specs/studio-device-profiles-v1.md sections 4 and 5).
//
// A device profile's hardware button or environment event reaches the running preview as ONE
// normalized event. It goes through the same `hal::SyntheticEventInjector` the native tests script
// (`press()` for the system buttons, `push()` for an environment event), is drained in the order
// `os-events-v1.md` section 3 fixes (OS events first, then input), and is applied to the session:
// Back and Volume through `ui::InputRouter`, lifecycle events to the app's life state, fold and
// orientation through the in-place layout the viewport already uses. There is no preview-only
// handler: the native `tessera_scene_preview --input-json` run and the Wasm `tessera_input_json`
// run call this very code, which is what the parity gate compares.
//
// **What the preview has no consumer for.** The preview evaluates one component; it has no route
// stack and no overlay host, so Back always resolves to "unhandled" and closes the app, exactly as
// on a one-route Android app. Theme, locale, text scale, IME, memory pressure, battery saver and
// thermal events are delivered and recorded, and answer `handled:false`: nothing in the evaluated
// tree reads them yet, and a reply that said otherwise would be a lie the event log then replays.
namespace tessera::compiler::codegen::cli {

// The normalized events a studio panel offers (the `controls` and `events` of a device profile).
enum class PlatformEventKind : std::uint8_t {
    Back,
    Home,
    Recents,
    VolumeUp,
    VolumeDown,
    Power,
    Suspend,
    Resume,
    Activate,
    ScreenOff,
    ScreenOn,
    Theme,
    Locale,
    TextScale,
    Orientation,
    Ime,
    Fold,
    MemoryPressure,
    BatterySaver,
    Thermal,
    ProcessDeath,
    Relaunch,
};

// The wire spelling (`"volumeUp"`, `"processDeath"`, ...). `platform_kind_from` is false for a name
// this build does not know, so a typo is an error reply and never a silent no-op.
[[nodiscard]] std::string_view platform_kind_name(PlatformEventKind kind) noexcept;
[[nodiscard]] bool platform_kind_from(std::string_view name, PlatformEventKind& out) noexcept;

// What the emulated app is doing, as the device would show it.
enum class AppLife : std::uint8_t {
    Running,    // foreground and focused
    Inactive,   // focus lost, still visible and running (Recents)
    Background, // hidden and suspended (Home): state kept, a Resume is a hot resume
    ScreenOff,  // the screen is off (Power)
    Closed,     // the process is gone (unhandled Back, process death)
};

[[nodiscard]] std::string_view app_life_name(AppLife life) noexcept;

// One event as the host sends it. `value` is the event's single argument (`landscape`, `dark`,
// `book`, ...); the wire carries it as `args.value`. `timestamp_ns` is the simulated clock; a
// negative value means "now" (the clock does not move).
struct PlatformEvent {
    PlatformEventKind kind = PlatformEventKind::Back;
    std::string value;
    std::int64_t timestamp_ns = -1;
};

struct PlatformReply {
    bool ok = true;
    std::string error; // why `ok` is false; a `TESSERA_PLATFORM_*` code
    PlatformEventKind kind = PlatformEventKind::Back;
    // The preview did something with the event (a Back that popped nothing is not handled).
    bool handled = false;
    // An unhandled Back closed the app, or a process death ended it.
    bool app_closed = false;
    // The app came back from Background, ScreenOff or Inactive with its state kept.
    bool resumed = false;
    AppLife state = AppLife::Running;
    // How a Back resolved: "overlay", "route", "unhandled", or "none" for every other event.
    std::string_view dispatch = "none";
    std::uint64_t sequence = 0;
    std::int64_t timestamp_ns = 0;
    // The event moved a layout input (orientation, fold), so a frame is owed.
    bool layout_changed = false;
    // A Suspend captured the state cells (the saved-state record of section 4).
    bool saved_state = false;
    // A Relaunch asks the caller to rebuild the tree; `restore` says to carry the saved cells into
    // it (process death after a Suspend) rather than start cold (after an unhandled Back).
    bool relaunch = false;
    bool restore = false;
};

// What an event is applied to. The driver never owns a tree: a bare `PreviewSession` (a scripted
// run) and the canvas session (which keeps its own copy of the viewport and posture, so a layout
// change has to go through it) each adapt themselves to this.
class PlatformTarget {
public:
    virtual ~PlatformTarget() = default;
    virtual bool set_viewport(float width, float height, float dpi_scale) = 0;
    virtual bool set_posture(hal::FoldPosture posture) = 0;
    [[nodiscard]] virtual const hal::WindowMetrics& metrics() const = 0;
    // The state cells, for the saved-state record at Suspend.
    [[nodiscard]] virtual CarriedState capture() const = 0;
    // Routes one input event through `ui::InputRouter` to the focused node.
    virtual ui::InputDispatchResult dispatch_input(const ui::InputEvent& event) = 0;
};

// The app's life state, the event injector and the saved-state record behind `platform.event`.
// Owned by whoever owns the session (the canvas session, or a scripted run); it holds no pointer
// to the session, which is passed per call, so a re-evaluation does not invalidate it.
class PlatformEventDriver {
public:
    // Delivers one event to `session`. Never throws; a refused event is a reply with `ok:false`.
    [[nodiscard]] PlatformReply deliver(PlatformTarget& target, const PlatformEvent& event);
    // The same for a bare session (the scripted run the parity gate compares).
    [[nodiscard]] PlatformReply deliver(PreviewSession& session, const PlatformEvent& event);

    [[nodiscard]] AppLife life() const noexcept {
        return life_;
    }
    // The cells captured at the last Suspend, for a relaunch after process death.
    [[nodiscard]] const std::optional<CarriedState>& saved() const noexcept {
        return saved_;
    }
    // A cold start: running, nothing saved. The injector's sequence and clock keep counting, so one
    // recorded log stays strictly ordered across a restart.
    void cold_start() noexcept;

private:
    // Applies the OS events and input the injector holds, in section 3's order.
    void drain(PlatformTarget& target, PlatformReply& reply);
    void apply_os_event(PlatformTarget& target, const hal::OsEvent& event, PlatformReply& reply);
    void apply_environment(PlatformTarget& target, const PlatformEvent& event,
                           PlatformReply& reply);

    hal::SyntheticEventInjector injector_;
    AppLife life_ = AppLife::Running;
    std::uint64_t sequence_ = 0;
    std::optional<CarriedState> saved_;
};

// One reply as the compact JSON object the wire carries (designer-protocol-v1 section 5.1).
[[nodiscard]] std::string platform_reply_json(const PlatformReply& reply);

} // namespace tessera::compiler::codegen::cli
