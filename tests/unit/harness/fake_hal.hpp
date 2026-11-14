// SPDX-License-Identifier: MIT

// TEST-ONLY doubles for the headless harness (docs/specs/headless-automation-v1.md section 2).
//
// This header lives under tests/ and is built into `tessera_harness_testing`, a library no
// shipping target links. A fake window never opens an OS window, creates a device or swapchain, or
// calls user code: the runner and its tests get normalized `hal::WindowMetrics`/`hal::WindowEvent`
// values and a manual clock they advance themselves, so a run is deterministic and never sleeps.

#pragma once

#include "tessera/hal/cutout.hpp"
#include "tessera/hal/window.hpp"
#include "tessera/harness/session.hpp"

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
    std::size_t dropped_events_ = 0;
    std::size_t redraw_requests_ = 0;
    bool visible_ = true;
    bool closed_ = false;
};

} // namespace tessera::harness::testing
