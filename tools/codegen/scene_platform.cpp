// SPDX-License-Identifier: MIT

#include "scene_platform.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "tessera/hal/os_event.hpp"
#include "tessera/ui/input.hpp"

#include <array>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

struct KindName {
    PlatformEventKind kind;
    std::string_view name;
};

constexpr std::array<KindName, 22> kKindNames{{
    {PlatformEventKind::Back, "back"},
    {PlatformEventKind::Home, "home"},
    {PlatformEventKind::Recents, "recents"},
    {PlatformEventKind::VolumeUp, "volumeUp"},
    {PlatformEventKind::VolumeDown, "volumeDown"},
    {PlatformEventKind::Power, "power"},
    {PlatformEventKind::Suspend, "suspend"},
    {PlatformEventKind::Resume, "resume"},
    {PlatformEventKind::Activate, "activate"},
    {PlatformEventKind::ScreenOff, "screenOff"},
    {PlatformEventKind::ScreenOn, "screenOn"},
    {PlatformEventKind::Theme, "theme"},
    {PlatformEventKind::Locale, "locale"},
    {PlatformEventKind::TextScale, "textScale"},
    {PlatformEventKind::Orientation, "orientation"},
    {PlatformEventKind::Ime, "ime"},
    {PlatformEventKind::Fold, "fold"},
    {PlatformEventKind::MemoryPressure, "memoryPressure"},
    {PlatformEventKind::BatterySaver, "batterySaver"},
    {PlatformEventKind::Thermal, "thermal"},
    {PlatformEventKind::ProcessDeath, "processDeath"},
    {PlatformEventKind::Relaunch, "relaunch"},
}};

// The OS event an environment kind is recorded as, so the event keeps its place in the injector's
// ordered stream. Kinds with no `OsEventType` (process death, relaunch) never reach the injector.
bool environment_os_type(PlatformEventKind kind, hal::OsEventType& out) noexcept {
    switch (kind) {
    case PlatformEventKind::Theme:
        out = hal::OsEventType::ThemeChanged;
        return true;
    case PlatformEventKind::Locale:
        out = hal::OsEventType::LocaleChanged;
        return true;
    case PlatformEventKind::TextScale:
        out = hal::OsEventType::TextScaleChanged;
        return true;
    case PlatformEventKind::Orientation:
        out = hal::OsEventType::OrientationChanged;
        return true;
    case PlatformEventKind::Ime:
        out = hal::OsEventType::ImeChanged;
        return true;
    case PlatformEventKind::Fold:
        out = hal::OsEventType::FoldChanged;
        return true;
    case PlatformEventKind::MemoryPressure:
        out = hal::OsEventType::MemoryPressure;
        return true;
    case PlatformEventKind::BatterySaver:
        out = hal::OsEventType::BatterySaverChanged;
        return true;
    case PlatformEventKind::Thermal:
        out = hal::OsEventType::ThermalStateChanged;
        return true;
    default:
        return false;
    }
}

bool accepts_input(AppLife life) noexcept {
    return life == AppLife::Running || life == AppLife::Inactive;
}

void fail(PlatformReply& reply, std::string error) {
    reply.ok = false;
    reply.error = std::move(error);
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            out.push_back('\\');
        }
        out.push_back(character);
    }
    out.push_back('"');
}

void append_flag(std::string& out, std::string_view name, bool value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += value ? "true" : "false";
}

} // namespace

std::string_view platform_kind_name(PlatformEventKind kind) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.kind == kind) {
            return entry.name;
        }
    }
    return "unknown";
}

bool platform_kind_from(std::string_view name, PlatformEventKind& out) noexcept {
    for (const KindName& entry : kKindNames) {
        if (entry.name == name) {
            out = entry.kind;
            return true;
        }
    }
    return false;
}

std::string_view app_life_name(AppLife life) noexcept {
    switch (life) {
    case AppLife::Running:
        return "running";
    case AppLife::Inactive:
        return "inactive";
    case AppLife::Background:
        return "background";
    case AppLife::ScreenOff:
        return "screenOff";
    case AppLife::Closed:
        return "closed";
    }
    return "unknown";
}

namespace {

// The adapter for a bare `PreviewSession`.
class SessionTarget final : public PlatformTarget {
public:
    explicit SessionTarget(PreviewSession& session)
        : session_(session) {}
    bool set_viewport(float width, float height, float dpi_scale) override {
        return session_.set_viewport(width, height, dpi_scale);
    }
    bool set_posture(hal::FoldPosture posture) override {
        return session_.set_posture(posture);
    }
    const hal::WindowMetrics& metrics() const override {
        return session_.metrics();
    }
    CarriedState capture() const override {
        return session_.capture();
    }
    ui::InputDispatchResult dispatch_input(const ui::InputEvent& event) override {
        return session_.router().dispatch(session_.root(), event);
    }

private:
    PreviewSession& session_;
};

} // namespace

PlatformReply PlatformEventDriver::deliver(PreviewSession& session, const PlatformEvent& event) {
    SessionTarget target(session);
    return deliver(target, event);
}

void PlatformEventDriver::cold_start() noexcept {
    life_ = AppLife::Running;
    saved_.reset();
}

PlatformReply PlatformEventDriver::deliver(PlatformTarget& target, const PlatformEvent& event) {
    PlatformReply reply;
    reply.kind = event.kind;
    if (event.timestamp_ns > injector_.now_ns()) {
        injector_.advance_ns(event.timestamp_ns - injector_.now_ns());
    }
    // A timestamp behind the clock is clamped, not refused: a replayed log is ordered by sequence,
    // and the clock is monotonic.
    reply.timestamp_ns = injector_.now_ns();
    reply.sequence = ++sequence_;

    if (event.kind == PlatformEventKind::Relaunch) {
        if (life_ != AppLife::Closed) {
            fail(reply, "TESSERA_PLATFORM_NOT_CLOSED: the app is running; there is nothing to "
                        "relaunch");
        } else {
            reply.relaunch = true;
            reply.restore = saved_.has_value();
            reply.handled = true;
            life_ = AppLife::Running;
        }
        reply.state = life_;
        return reply;
    }
    if (life_ == AppLife::Closed) {
        // A dead process receives nothing; only a relaunch brings it back.
        reply.state = life_;
        return reply;
    }
    if (event.kind == PlatformEventKind::ProcessDeath) {
        life_ = AppLife::Closed;
        reply.app_closed = true;
        reply.handled = true;
        reply.saved_state = saved_.has_value();
        reply.state = life_;
        return reply;
    }

    using hal::SystemButton;
    bool queued = true;
    switch (event.kind) {
    case PlatformEventKind::Back:
    case PlatformEventKind::VolumeUp:
    case PlatformEventKind::VolumeDown:
        // A screen that is off or an app that is in the background receives no key.
        queued = accepts_input(life_);
        if (queued) {
            const SystemButton button =
                event.kind == PlatformEventKind::Back
                    ? SystemButton::Back
                    : (event.kind == PlatformEventKind::VolumeUp ? SystemButton::VolumeUp
                                                                 : SystemButton::VolumeDown);
            static_cast<void>(injector_.press(button));
        }
        break;
    case PlatformEventKind::Home:
        static_cast<void>(injector_.press(SystemButton::Home));
        break;
    case PlatformEventKind::Recents:
        static_cast<void>(injector_.press(SystemButton::Recents));
        break;
    case PlatformEventKind::Power:
        // The toggle follows the app's life, so Power after an explicit ScreenOff wakes the screen.
        static_cast<void>(injector_.push(life_ == AppLife::ScreenOff ? hal::OsEventType::ScreenOn
                                                                     : hal::OsEventType::ScreenOff,
                                         hal::EventReason::User));
        break;
    case PlatformEventKind::Suspend:
        static_cast<void>(injector_.push(hal::OsEventType::Suspend, hal::EventReason::User));
        break;
    case PlatformEventKind::Resume:
        static_cast<void>(injector_.push(hal::OsEventType::Resume, hal::EventReason::User));
        static_cast<void>(injector_.push(hal::OsEventType::WindowShown, hal::EventReason::User));
        break;
    case PlatformEventKind::Activate:
        static_cast<void>(
            injector_.push(hal::OsEventType::WindowActivated, hal::EventReason::User));
        break;
    case PlatformEventKind::ScreenOff:
        static_cast<void>(injector_.push(hal::OsEventType::ScreenOff, hal::EventReason::User));
        break;
    case PlatformEventKind::ScreenOn:
        static_cast<void>(injector_.push(hal::OsEventType::ScreenOn, hal::EventReason::User));
        break;
    default: {
        hal::OsEventType type = hal::OsEventType::ThemeChanged;
        if (environment_os_type(event.kind, type)) {
            static_cast<void>(injector_.push(type, hal::EventReason::User));
        }
        break;
    }
    }

    if (queued) {
        drain(target, reply);
    }
    hal::OsEventType environment = hal::OsEventType::ThemeChanged;
    if (environment_os_type(event.kind, environment)) {
        apply_environment(target, event, reply);
    }
    reply.state = life_;
    return reply;
}

void PlatformEventDriver::drain(PlatformTarget& target, PlatformReply& reply) {
    // os-events-v1.md section 3: every OS event of the tick first, then input.
    hal::OsEvent os_event;
    while (injector_.pop(os_event)) {
        apply_os_event(target, os_event, reply);
    }
    ui::InputEvent input;
    while (injector_.pop_input(input)) {
        if (input.type == ui::InputEventType::BackRequested) {
            // No overlay host and no route stack in the preview: nothing takes Back, so the app
            // closes and its state is discarded (the spec's rule 1).
            reply.dispatch = "unhandled";
            reply.app_closed = true;
            reply.handled = true;
            life_ = AppLife::Closed;
            saved_.reset();
        } else if (input.type == ui::InputEventType::VolumeKey) {
            const ui::InputDispatchResult result = target.dispatch_input(input);
            reply.handled = result.dispatched;
        }
    }
}

void PlatformEventDriver::apply_os_event(PlatformTarget& target, const hal::OsEvent& event,
                                         PlatformReply& reply) {
    switch (event.type) {
    case hal::OsEventType::Suspend:
        if (accepts_input(life_)) {
            life_ = AppLife::Background;
            // Captured at Suspend, the last event a process is certain to see (os-events-v1.md
            // section 2.3); a hot resume never reads it.
            saved_ = target.capture();
            reply.saved_state = true;
            reply.handled = true;
        }
        break;
    case hal::OsEventType::Resume:
        if (life_ == AppLife::Background) {
            life_ = AppLife::Running;
            reply.resumed = true;
            reply.handled = true;
        }
        break;
    case hal::OsEventType::WindowDeactivated:
        if (life_ == AppLife::Running) {
            life_ = AppLife::Inactive;
            reply.handled = true;
        }
        break;
    case hal::OsEventType::WindowActivated:
        if (life_ == AppLife::Inactive) {
            life_ = AppLife::Running;
            reply.resumed = true;
            reply.handled = true;
        }
        break;
    case hal::OsEventType::ScreenOff:
        if (life_ == AppLife::Running || life_ == AppLife::Inactive) {
            life_ = AppLife::ScreenOff;
            reply.handled = true;
        }
        break;
    case hal::OsEventType::ScreenOn:
        if (life_ == AppLife::ScreenOff) {
            life_ = AppLife::Running;
            reply.resumed = true;
            reply.handled = true;
        }
        break;
    default:
        // WindowHidden and WindowShown ride with Suspend and Resume; the environment kinds are
        // applied from their argument by `apply_environment`.
        break;
    }
}

void PlatformEventDriver::apply_environment(PlatformTarget& target, const PlatformEvent& event,
                                            PlatformReply& reply) {
    const hal::WindowMetrics& metrics = target.metrics();
    if (event.kind == PlatformEventKind::Orientation) {
        if (event.value != "portrait" && event.value != "landscape") {
            fail(reply, "TESSERA_PLATFORM_BAD_VALUE: orientation expects portrait or landscape");
            return;
        }
        const auto width = static_cast<float>(metrics.logical_width);
        const auto height = static_cast<float>(metrics.logical_height);
        const bool landscape = event.value == "landscape";
        const bool swap = (width >= height) != landscape;
        reply.layout_changed = swap ? target.set_viewport(height, width, metrics.dpi_scale) : false;
        reply.handled = true;
    } else if (event.kind == PlatformEventKind::Fold) {
        hal::FoldPosture posture = hal::FoldPosture::None;
        if (!event.value.empty() && event.value != "none") {
            const std::optional<hal::FoldPosture> parsed = hal::parse_fold_posture(event.value);
            if (!parsed) {
                fail(reply, "TESSERA_PLATFORM_BAD_VALUE: fold expects none, flat, book or "
                            "tabletop");
                return;
            }
            posture = *parsed;
        }
        reply.layout_changed = target.set_posture(posture);
        reply.handled = true;
    }
    // Theme, locale, text scale, IME, memory pressure, battery saver and thermal state have no
    // consumer in an evaluated preview tree: delivered, recorded, `handled:false`.
}

std::string platform_reply_json(const PlatformReply& reply) {
    std::string out = "{\"ok\":";
    out += reply.ok ? "true" : "false";
    if (!reply.ok) {
        out += ",\"error\":";
        append_quoted(out, reply.error);
    }
    out += ",\"type\":";
    append_quoted(out, platform_kind_name(reply.kind));
    append_flag(out, "handled", reply.handled);
    append_flag(out, "appClosed", reply.app_closed);
    append_flag(out, "resumed", reply.resumed);
    out += ",\"state\":";
    append_quoted(out, app_life_name(reply.state));
    out += ",\"dispatch\":";
    append_quoted(out, reply.dispatch);
    out += ",\"sequence\":";
    out += std::to_string(reply.sequence);
    out += ",\"timestampNs\":";
    out += std::to_string(reply.timestamp_ns);
    append_flag(out, "changed", reply.layout_changed);
    append_flag(out, "savedState", reply.saved_state);
    append_flag(out, "relaunch", reply.relaunch);
    append_flag(out, "restore", reply.restore);
    out.push_back('}');
    return out;
}

} // namespace tessera::compiler::codegen::cli
