// SPDX-License-Identifier: MIT
//
// The synthetic event injector and the named display-capability scripts
// (docs/specs/os-events-v1.md sections 3 and 12; ADR-023 rule 8).
//
// Determinism is the whole point: the queue is a bounded ring, the clock is simulated, the sequence
// is a counter, and the coalescing rule is the same one the platform queue applies. A replay from
// the same script produces the same events in the same order with the same timestamps on every
// machine, which is what lets the acceptance criteria say "synthetic display scripts pass on
// Windows and Linux".

#include "tessera/hal/synthetic_events.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::hal {
namespace {

// A metrics snapshot whose display fields follow a script change. Only the fields the event's type
// owns are copied, so a refresh change never silently rewrites the work area.
void apply_change(WindowMetrics& metrics, const DisplayChange& change) {
    if (change.display != kNoDisplayId) {
        metrics.display = change.display;
    }
    switch (change.type) {
    case OsEventType::RefreshRateChanged:
        if (std::isfinite(change.refresh_hz) && change.refresh_hz > 0.0f) {
            metrics.refresh_hz = change.refresh_hz;
            metrics.assumed.refresh_hz = false;
        }
        break;
    case OsEventType::VrrChanged:
        metrics.flags.vrr = change.reason == EventReason::Programmatic;
        metrics.assumed.vrr = false;
        break;
    case OsEventType::DpiChanged:
    case OsEventType::ScaleChanged:
        metrics.dpi_scale = resolve_display_scale(change.dpi_scale);
        break;
    case OsEventType::WorkAreaChanged:
        metrics.work_area =
            resolve_work_area(change.work_area, metrics.logical_width, metrics.logical_height);
        metrics.assumed.work_area = false;
        break;
    case OsEventType::WindowResized:
        if (change.physical_width > 0 && change.physical_height > 0) {
            metrics.physical_width = change.physical_width;
            metrics.physical_height = change.physical_height;
            metrics.logical_width = static_cast<std::uint32_t>(
                std::lround(static_cast<double>(change.physical_width) / metrics.dpi_scale));
            metrics.logical_height = static_cast<std::uint32_t>(
                std::lround(static_cast<double>(change.physical_height) / metrics.dpi_scale));
            metrics.orientation =
                orientation_from_extent(metrics.logical_width, metrics.logical_height);
        }
        break;
    default:
        break;
    }
}

} // namespace

void SyntheticEventInjector::advance_ns(std::int64_t delta) noexcept {
    if (delta <= 0) {
        return;
    }
    // Saturate instead of overflowing into a negative timestamp: a script that runs for years of
    // simulated time still produces ordered, non-negative stamps.
    constexpr std::int64_t kMax = 9223372036854775807LL;
    now_ns_ = delta > kMax - now_ns_ ? kMax : now_ns_ + delta;
}

void SyntheticEventInjector::advance_ms(std::int64_t delta) noexcept {
    constexpr std::int64_t kMaxMs = 9223372036854775807LL / 1000000LL;
    advance_ns(std::min(delta, kMaxMs) * 1000000LL);
}

void SyntheticEventInjector::clear() noexcept {
    head_ = 0;
    count_ = 0;
    input_head_ = 0;
    input_count_ = 0;
    screen_on_ = true;
    overflows_ = 0;
    coalesced_ = 0;
}

InjectStatus SyntheticEventInjector::record(OsEvent event) {
    if (count_ > 0 && os_event_coalesces(event.type)) {
        // Walk back to the newest event of this type; if the tail is that type, merge into it. The
        // platform queue only merges a contiguous run, and so does this: an event in between is a
        // real ordering boundary a script asked for.
        OsEvent& newest = events_[(head_ + count_ - 1) % events_.size()];
        if (newest.type == event.type) {
            const EventReason reason = merge_event_reason(newest.reason, event.reason);
            newest = event;
            newest.reason = reason;
            ++coalesced_;
            return InjectStatus::Queued;
        }
    }
    if (count_ == events_.size()) {
        ++overflows_;
        return InjectStatus::Full;
    }
    events_[(head_ + count_) % events_.size()] = event;
    ++count_;
    return InjectStatus::Queued;
}

InjectStatus SyntheticEventInjector::push(OsEventType type, EventReason reason) {
    return push(type, metrics_, reason);
}

InjectStatus SyntheticEventInjector::push(OsEventType type, const WindowMetrics& metrics,
                                          EventReason reason) {
    if (!os_event_is_known(type)) {
        // Rule 5: an unknown event is ignored. It is not an overflow, so it does not count as one.
        return InjectStatus::UnknownType;
    }
    OsEvent event;
    event.type = type;
    event.sequence = sequence_.next();
    event.timestamp_ns = now_ns_;
    event.window = kPrimaryWindowId;
    event.display = metrics.display;
    event.metrics = metrics;
    event.fold = metrics.fold;
    event.reason = reason;
    event.handled = false;
    return record(event);
}

InjectStatus SyntheticEventInjector::apply(const DisplayChange& change) {
    if (!os_event_is_known(change.type)) {
        return InjectStatus::UnknownType;
    }
    apply_change(metrics_, change);
    // The event the platform would have emitted carries the state the change produced. The null
    // display id in the change means "the window's current display", which is what a real backend
    // reports when the display did not change.
    return push(change.type, metrics_, change.reason);
}

bool SyntheticEventInjector::pop(OsEvent& out) noexcept {
    if (count_ == 0) {
        return false;
    }
    out = events_[head_];
    head_ = (head_ + 1) % events_.size();
    --count_;
    return true;
}

std::size_t SyntheticEventInjector::drain(std::span<OsEvent> out) noexcept {
    const std::size_t copied = std::min(out.size(), count_);
    for (std::size_t index = 0; index < copied; ++index) {
        out[index] = events_[(head_ + index) % events_.size()];
    }
    head_ = (head_ + copied) % events_.size();
    count_ -= copied;
    return copied;
}

namespace {

// A 1920x1080 logical window at 1.0 scale, 60 Hz, on display 1.
WindowMetrics base_metrics() {
    WindowMetrics metrics;
    metrics.logical_width = 1920;
    metrics.logical_height = 1080;
    metrics.physical_width = 1920;
    metrics.physical_height = 1080;
    metrics.dpi_scale = 1.0f;
    metrics.display = 1;
    metrics.refresh_hz = 60.0f;
    metrics.assumed.refresh_hz = false;
    metrics.work_area = WindowRect{0.0f, 0.0f, 1920.0f, 1040.0f};
    metrics.assumed.work_area = false;
    metrics.orientation = Orientation::Landscape;
    return metrics;
}

} // namespace

SyntheticEventInjector make_resize_dpi_storm_script() {
    SyntheticEventInjector injector;
    injector.set_metrics(base_metrics());
    // A live drag at 1.0 scale: many intermediate sizes, then a DPI change to 150 % with the
    // suggested rect, then more resizes at the new scale. The window path coalesces each run into
    // one event, so the queue holds two: the resize (latest) and the DPI change.
    for (std::uint32_t step = 0; step < 8; ++step) {
        DisplayChange resize;
        resize.type = OsEventType::WindowResized;
        resize.reason = EventReason::User;
        resize.physical_width = 1400 + step * 20;
        resize.physical_height = 900 + step * 10;
        (void)injector.apply(resize);
        injector.advance_ms(16);
    }
    DisplayChange dpi;
    dpi.type = OsEventType::DpiChanged;
    dpi.reason = EventReason::Dpi;
    dpi.dpi_scale = 1.5f;
    dpi.physical_width = 2100;
    dpi.physical_height = 1350;
    (void)injector.apply(dpi);
    injector.advance_ms(16);
    for (std::uint32_t step = 0; step < 4; ++step) {
        DisplayChange resize;
        resize.type = OsEventType::WindowResized;
        resize.reason = EventReason::User;
        resize.physical_width = 2100 + step * 30;
        resize.physical_height = 1350 + step * 20;
        (void)injector.apply(resize);
        injector.advance_ms(16);
    }
    // The drag ends: a DPI event would have coalesced, but the exit is a distinct state change, so
    // the script emits it as an explicit resize with the programmatic reason.
    DisplayChange settled;
    settled.type = OsEventType::WindowResized;
    settled.reason = EventReason::Programmatic;
    settled.physical_width = 2100 + 3 * 30;
    settled.physical_height = 1350 + 3 * 20;
    (void)injector.apply(settled);
    return injector;
}

SyntheticEventInjector make_display_hotplug_script() {
    SyntheticEventInjector injector;
    injector.set_metrics(base_metrics());
    // The monitor is unplugged: the OS reports the removal and the window lands on the remaining
    // display, which runs at 120 Hz and 2x scale.
    DisplayChange removed;
    removed.type = OsEventType::DisplayRemoved;
    removed.reason = EventReason::Hotplug;
    removed.display = 1;
    (void)injector.apply(removed);
    injector.advance_ms(50);

    DisplayChange moved;
    moved.type = OsEventType::DisplayChanged;
    moved.reason = EventReason::Hotplug;
    moved.display = 2;
    moved.dpi_scale = 2.0f;
    moved.refresh_hz = 120.0f;
    (void)injector.apply(moved);
    injector.advance_ms(50);

    DisplayChange scale;
    scale.type = OsEventType::ScaleChanged;
    scale.reason = EventReason::Display;
    scale.display = 2;
    scale.dpi_scale = 2.0f;
    (void)injector.apply(scale);

    DisplayChange refresh;
    refresh.type = OsEventType::RefreshRateChanged;
    refresh.reason = EventReason::Display;
    refresh.display = 2;
    refresh.refresh_hz = 120.0f;
    (void)injector.apply(refresh);
    return injector;
}

SyntheticEventInjector make_scale_change_script() {
    SyntheticEventInjector injector;
    injector.set_metrics(base_metrics());
    // A compositor scale change: the client area keeps its physical pixels, so the logical size
    // follows the new scale; the work area is reported in the new logical space too.
    DisplayChange scaled;
    scaled.type = OsEventType::ScaleChanged;
    scaled.reason = EventReason::System;
    scaled.dpi_scale = 2.0f;
    scaled.work_area = WindowRect{0.0f, 0.0f, 960.0f, 520.0f};
    (void)injector.apply(scaled);
    injector.advance_ms(16);
    // A second change back to 1.0: coalescing must keep the newest scale, not the first.
    DisplayChange restored;
    restored.type = OsEventType::ScaleChanged;
    restored.reason = EventReason::System;
    restored.dpi_scale = 1.0f;
    (void)injector.apply(restored);
    return injector;
}

SyntheticEventInjector make_refresh_rate_change_script() {
    SyntheticEventInjector injector;
    injector.set_metrics(base_metrics());
    DisplayChange high;
    high.type = OsEventType::RefreshRateChanged;
    high.reason = EventReason::Display;
    high.refresh_hz = 120.0f;
    (void)injector.apply(high);
    injector.advance_ms(16);
    // A mode that runs at 59.94 Hz: the value is carried as reported, not rounded to 60.
    DisplayChange ntsc;
    ntsc.type = OsEventType::RefreshRateChanged;
    ntsc.reason = EventReason::Display;
    ntsc.refresh_hz = 59.94f;
    (void)injector.apply(ntsc);
    return injector;
}

} // namespace tessera::hal
