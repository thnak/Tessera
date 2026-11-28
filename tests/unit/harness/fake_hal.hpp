// SPDX-License-Identifier: MIT

// TEST-ONLY doubles for the headless harness (docs/specs/headless-automation-v1.md section 2).
//
// This header lives under tests/ and is built into `tessera_harness_testing`, a library no
// shipping target links. A fake window never opens an OS window, creates a device or swapchain, or
// calls user code: the runner and its tests get normalized `hal::WindowMetrics`/`hal::WindowEvent`
// values and a manual clock they advance themselves, so a run is deterministic and never sleeps.

#pragma once

#include "tessera/hal/cutout.hpp"
#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/harness/session.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>

namespace tessera::harness::testing {

// A window script cannot queue unbounded events through the fake: the cap is enforced by dropping
// the oldest event and counting it, exactly like a real platform queue under pressure.
inline constexpr std::size_t kMaxFakeWindowEvents = 64;

// Deterministic simulation clock for the runner's budgets; advances only when the script/host says
// so. The runner never reads a wall clock, so elapsed time is reproducible across machines.
class ManualClock final : public SessionClock {
public:
    explicit ManualClock(std::uint64_t start_us = 0) noexcept;

    [[nodiscard]] std::uint64_t now_us() const override;
    void advance_us(std::uint64_t delta) noexcept;
    void advance_ms(std::uint64_t delta) noexcept;

private:
    std::uint64_t now_us_ = 0;
};

[[nodiscard]] hal::WindowMetrics default_fake_metrics() noexcept;

// Implements the existing `hal::Window` seam. Metrics are normalized logical/physical pixels plus
// DPI scale; events are the same `WindowEvent` values a platform backend produces.
class FakeWindow final : public hal::Window {
public:
    explicit FakeWindow(hal::WindowMetrics metrics = default_fake_metrics()) noexcept;

    FakeWindow(const FakeWindow&) = delete;
    FakeWindow& operator=(const FakeWindow&) = delete;

    [[nodiscard]] hal::WindowMetrics metrics() const override;
    std::size_t poll_events(std::span<hal::WindowEvent> events) override;
    // Never waits and never sleeps: reports whether an event is already queued. Returning without
    // consuming anything keeps `wait_for_events_until` deterministic in tests.
    bool wait_for_events(double timeout_seconds) override;
    void show() override;
    void hide() override;
    void request_redraw() override;
    void close() override;
    // Full-screen (issue #232): the fake resolves the request synchronously and reports it through
    // the metrics, exactly like a platform whose window manager honored it. A scripted refusal
    // (`set_fullscreen_supported(false)`) models the "no window manager" path a real backend
    // reports.
    void set_fullscreen(bool fullscreen) override;
    [[nodiscard]] bool supports_fullscreen() const noexcept override {
        return fullscreen_supported_;
    }
    void set_caption_regions(std::span<const hal::CaptionRegion> regions) override;
    // Pointer shape (issue #191): the fake resolves the request synchronously and reports it
    // through the metrics, exactly like a platform that installed the native cursor. A scripted
    // refusal (`set_cursor_supported(false)`) models a platform with no pointer-shape source, which
    // is what the app's one-shot diagnostic decides from.
    void set_cursor(hal::CursorShape shape) override;
    // Records the IME anchor requests (issue #1053) so a test can prove the app publishes the caret
    // rect and only on a change.
    void set_text_input_rect(const hal::TextInputRect& rect) override;
    [[nodiscard]] std::size_t text_input_rect_requests() const noexcept {
        return text_input_rect_requests_;
    }
    [[nodiscard]] const hal::TextInputRect& last_text_input_rect() const noexcept {
        return last_text_input_rect_;
    }
    [[nodiscard]] bool supports_cursor() const noexcept override {
        return cursor_supported_;
    }
    // Accessibility root seam (issues #185/#186): the fake stores the installed answer and reports
    // a non-null opaque host token, so an app-level accessibility test can drive the real wiring
    // without an OS window. A scripted null token (`set_accessibility_host_handle({})`) models a
    // backend with no native host element (X11).
    void set_accessibility_root(hal::AccessibilityRoot root) override;
    [[nodiscard]] hal::AccessibilityHostHandle accessibility_host_handle() const override;
    void set_accessibility_host_handle(hal::AccessibilityHostHandle handle) noexcept;
    // The installed root answer (null `answer` when nothing is installed); the app test asserts the
    // install/uninstall order through it.
    [[nodiscard]] const hal::AccessibilityRoot& accessibility_root() const noexcept {
        return accessibility_root_;
    }
    [[nodiscard]] bool accessibility_root_installed() const noexcept {
        return accessibility_root_.answer != nullptr;
    }
    [[nodiscard]] hal::NativeWindowHandle native_handle() const override {
        return {};
    }

    // Scripted changes. `resize` derives physical pixels from the logical size and the DPI scale,
    // mirroring what a platform backend reports after a resize.
    void set_metrics(hal::WindowMetrics metrics) noexcept;
    void resize(std::uint32_t logical_width, std::uint32_t logical_height, double dpi_scale = 1.0);
    void set_dpi_scale(double dpi_scale);
    void set_focused(bool focused);
    void set_occluded(bool occluded);
    void set_minimized(bool minimized);
    void push_event(hal::WindowEventType type);
    // Injects insets and cutouts the way Android reports them (`make_cutout_info` caps the list at
    // `kMaxCutouts`) and queues one SafeAreaChanged carrying the new metrics.
    void set_safe_area(const hal::WindowInsets& insets, std::span<const hal::WindowRect> cutouts,
                       const hal::WindowInsets& waterfall = {});
    // Injects the keyboard and gesture insets and queues one ImeChanged (coalescing).
    void set_ime_insets(const hal::WindowInsets& ime, const hal::WindowInsets& gestures = {});
    // Injects the fold and queues one FoldChanged (coalescing).
    void set_fold(const hal::FoldInfo& fold);
    // Scripted platform capability: false models a display with no window manager, where a
    // full-screen request is refused and the window stays as it was (issue #232).
    void set_fullscreen_supported(bool supported) noexcept;
    // Scripted platform capability: false models a platform with no pointer-shape source, where a
    // cursor request is ignored and the metrics keep the previous shape (issue #191).
    void set_cursor_supported(bool supported) noexcept;
    // Counts `set_cursor` calls, so a test can prove the app only asks on a change.
    [[nodiscard]] std::size_t cursor_requests() const noexcept;

    // --- display capabilities (docs/specs/os-events-v1.md section 2, ADR-023 rules 4 and 7) -----
    // The OS appearance snapshot (issue #187). It is the same normalized value a real platform
    // reports, so the app driver's `ThemeChanged`/`SystemSettingsChanged` handling is tested
    // without a desktop: `set_appearance` records a change and queues the event the platform would
    // send.
    [[nodiscard]] hal::AppearancePreference appearance() const noexcept override {
        return appearance_;
    }
    // Records the new snapshot and queues the normalized event. `measured` models a platform that
    // answered; the default `AppearancePreference` is the documented fallback with both
    // assumptions.
    void set_appearance(hal::AppearancePreference appearance);
    // Records the snapshot WITHOUT queueing an event, so a test can prove the host only reacts to
    // the event (the platform's "re-read at the tick boundary" contract).
    void seed_appearance(hal::AppearancePreference appearance) noexcept;

    // --- display capabilities (docs/specs/os-events-v1.md section 2, ADR-023 rules 4 and 7) -----
    // Sets the display the window is on and the refresh/VRR the platform reports. `assumed` models
    // the documented fallback (a platform that cannot report the capability): the metrics keep
    // kAssumedDisplayRefreshHz/kAssumedDisplayScale and set the matching flag, which is what a
    // consumer must be able to see instead of a value presented as measured.
    void set_display(hal::DisplayId id, float refresh_hz, bool vrr = false);
    // Records a refresh change and queues one `RefreshRateChanged` carrying the new metrics; a
    // consecutive one coalesces like a platform queue.
    void set_refresh_rate(float refresh_hz, bool assumed = false);
    // Records a work-area change and queues one `WorkAreaChanged` (coalescing).
    void set_work_area(const hal::WindowRect& work_area);
    // Records the per-monitor topology the fake reports from `displays()`. Capped at
    // `hal::kMaxDisplays`; the extras are dropped. A test scripts a hotplug by replacing it and
    // queuing `DisplayAdded`/`DisplayRemoved`.
    void set_displays(std::span<const hal::DisplayInfo> displays);
    // Queues the display event a script wants and, for an add/remove, applies the topology change
    // first so the event's metrics and the list agree.
    void push_display_event(hal::WindowEventType type, hal::DisplayId display = hal::kNoDisplayId);
    [[nodiscard]] std::size_t displays(std::span<hal::DisplayInfo> displays) const override;

    [[nodiscard]] bool has_queued(hal::WindowEventType type) const noexcept;
    [[nodiscard]] std::size_t queued_events() const noexcept;
    [[nodiscard]] std::size_t dropped_events() const noexcept;
    [[nodiscard]] std::size_t redraw_requests() const noexcept;
    [[nodiscard]] bool visible() const noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    void push_metrics_event(hal::WindowEventType type);
    void update_physical_size();

    hal::WindowMetrics metrics_;
    std::deque<hal::WindowEvent> events_;
    // The OS appearance snapshot the fake reports (issue #187); the documented fallback until a
    // test scripts one, which is exactly what a platform with no settings source answers.
    hal::AppearancePreference appearance_;
    // The per-monitor topology `displays()` reports; a fake with no topology source leaves it empty
    // and reports 0, which is the documented "no per-monitor information" answer.
    std::array<hal::DisplayInfo, hal::kMaxDisplays> displays_{};
    std::size_t display_count_ = 0;
    std::size_t dropped_events_ = 0;
    std::size_t redraw_requests_ = 0;
    bool visible_ = true;
    bool closed_ = false;
    bool fullscreen_supported_ = true;
    bool cursor_supported_ = true;
    std::size_t cursor_requests_ = 0;
    std::size_t text_input_rect_requests_ = 0;
    hal::TextInputRect last_text_input_rect_{};
    // Defaults to a non-null opaque token, like the Win32 window's HWND; a test that models a
    // backend without a native host element clears it.
    hal::AccessibilityHostHandle accessibility_handle_{reinterpret_cast<void*>(1)};
    hal::AccessibilityRoot accessibility_root_{};
};

} // namespace tessera::harness::testing
