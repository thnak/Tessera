// SPDX-License-Identifier: MIT

#include "fake_hal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace tessera::harness::testing {
namespace {

std::uint32_t to_physical(std::uint32_t logical, double scale) noexcept {
    const double scaled = std::round(static_cast<double>(logical) * scale);
    if (!std::isfinite(scaled) || scaled <= 0.0) {
        return logical;
    }
    const double capped =
        std::min(scaled, static_cast<double>(std::numeric_limits<std::uint32_t>::max()));
    return static_cast<std::uint32_t>(capped);
}

} // namespace

ManualClock::ManualClock(std::uint64_t start_us) noexcept
    : now_us_(start_us) {}

std::uint64_t ManualClock::now_us() const {
    return now_us_;
}

void ManualClock::advance_us(std::uint64_t delta) noexcept {
    now_us_ = delta > std::numeric_limits<std::uint64_t>::max() - now_us_
                  ? std::numeric_limits<std::uint64_t>::max()
                  : now_us_ + delta;
}

void ManualClock::advance_ms(std::uint64_t delta) noexcept {
    constexpr std::uint64_t kMaxMs = std::numeric_limits<std::uint64_t>::max() / 1000U;
    advance_us(std::min(delta, kMaxMs) * 1000U);
}

hal::WindowMetrics default_fake_metrics() noexcept {
    hal::WindowMetrics metrics;
    metrics.logical_width = 1280;
    metrics.logical_height = 720;
    metrics.physical_width = 1280;
    metrics.physical_height = 720;
    metrics.dpi_scale = 1.0f;
    metrics.focused = true;
    return metrics;
}

FakeWindow::FakeWindow(hal::WindowMetrics metrics) noexcept
    : metrics_(metrics) {}

hal::WindowMetrics FakeWindow::metrics() const {
    return metrics_;
}

std::size_t FakeWindow::poll_events(std::span<hal::WindowEvent> events) {
    const std::size_t count = std::min(events.size(), events_.size());
    for (std::size_t index = 0; index < count; ++index) {
        events[index] = events_.front();
        events_.pop_front();
    }
    return count;
}

bool FakeWindow::wait_for_events(double timeout_seconds) {
    (void)timeout_seconds;
    return !events_.empty();
}

void FakeWindow::show() {
    visible_ = true;
}

void FakeWindow::hide() {
    visible_ = false;
}

void FakeWindow::request_redraw() {
    ++redraw_requests_;
}

void FakeWindow::close() {
    if (closed_) {
        return;
    }
    closed_ = true;
    push_event(hal::WindowEventType::Close);
}

void FakeWindow::set_fullscreen(bool fullscreen) {
    // A platform without a window manager refuses the request: the mode and the geometry stay as
    // they were, which is exactly what the real backend reports through _NET_WM_STATE.
    if (!fullscreen_supported_ || metrics_.fullscreen == fullscreen) {
        return;
    }
    // The fake models the resolved state: the OS applies the mode synchronously and reports it with
    // the new metrics. It does not invent a size, because a real platform's full-screen rectangle
    // depends on the display; a test that cares about geometry sets it explicitly.
    metrics_.fullscreen = fullscreen;
    push_metrics_event(hal::WindowEventType::Resize);
}

void FakeWindow::set_caption_regions(std::span<const hal::CaptionRegion> regions) {
    metrics_.caption_regions = hal::make_caption_regions(regions, metrics_.caption_region_count);
}

void FakeWindow::set_fullscreen_supported(bool supported) noexcept {
    fullscreen_supported_ = supported;
}

void FakeWindow::set_cursor(hal::CursorShape shape) {
    ++cursor_requests_;
    // A platform with no pointer-shape source ignores the request: the resolved shape stays as it
    // was, which is exactly what the app's unsupported diagnostic is for.
    if (!cursor_supported_) {
        return;
    }
    metrics_.cursor = shape;
}

void FakeWindow::set_cursor_supported(bool supported) noexcept {
    cursor_supported_ = supported;
}

std::size_t FakeWindow::cursor_requests() const noexcept {
    return cursor_requests_;
}

void FakeWindow::set_metrics(hal::WindowMetrics metrics) noexcept {
    metrics_ = metrics;
}

void FakeWindow::resize(std::uint32_t logical_width, std::uint32_t logical_height,
                        double dpi_scale) {
    if (logical_width == 0 || logical_height == 0 || !std::isfinite(dpi_scale) ||
        !(dpi_scale > 0.0)) {
        return;
    }
    metrics_.logical_width = logical_width;
    metrics_.logical_height = logical_height;
    metrics_.dpi_scale = static_cast<float>(dpi_scale);
    update_physical_size();
    push_metrics_event(hal::WindowEventType::Resize);
}

void FakeWindow::set_dpi_scale(double dpi_scale) {
    if (!std::isfinite(dpi_scale) || !(dpi_scale > 0.0)) {
        return;
    }
    metrics_.dpi_scale = static_cast<float>(dpi_scale);
    update_physical_size();
    push_metrics_event(hal::WindowEventType::DpiChange);
}

void FakeWindow::set_focused(bool focused) {
    if (metrics_.focused == focused) {
        return;
    }
    metrics_.focused = focused;
    push_metrics_event(focused ? hal::WindowEventType::Focus : hal::WindowEventType::Blur);
}

void FakeWindow::set_occluded(bool occluded) {
    if (metrics_.occluded == occluded) {
        return;
    }
    metrics_.occluded = occluded;
    push_metrics_event(occluded ? hal::WindowEventType::Occluded : hal::WindowEventType::Restored);
}

void FakeWindow::set_minimized(bool minimized) {
    if (metrics_.minimized == minimized) {
        return;
    }
    metrics_.minimized = minimized;
    push_metrics_event(minimized ? hal::WindowEventType::Minimized
                                 : hal::WindowEventType::Restored);
}

void FakeWindow::set_safe_area(const hal::WindowInsets& insets,
                               std::span<const hal::WindowRect> cutouts,
                               const hal::WindowInsets& waterfall) {
    metrics_.insets = insets;
    metrics_.cutouts = hal::make_cutout_info(cutouts, waterfall);
    // Consecutive safe-area changes coalesce into the newest, like a platform queue.
    if (!events_.empty() && events_.back().type == hal::WindowEventType::SafeAreaChanged) {
        events_.back().metrics = metrics_;
        return;
    }
    push_metrics_event(hal::WindowEventType::SafeAreaChanged);
}

void FakeWindow::set_ime_insets(const hal::WindowInsets& ime, const hal::WindowInsets& gestures) {
    metrics_.ime_insets = ime;
    metrics_.gesture_insets = gestures;
    if (!events_.empty() && events_.back().type == hal::WindowEventType::ImeChanged) {
        events_.back().metrics = metrics_;
        return;
    }
    push_metrics_event(hal::WindowEventType::ImeChanged);
}

void FakeWindow::set_fold(const hal::FoldInfo& fold) {
    metrics_.fold = fold;
    if (!events_.empty() && events_.back().type == hal::WindowEventType::FoldChanged) {
        events_.back().metrics = metrics_;
        return;
    }
    push_metrics_event(hal::WindowEventType::FoldChanged);
}

void FakeWindow::push_event(hal::WindowEventType type) {
    push_metrics_event(type);
}

bool FakeWindow::has_queued(hal::WindowEventType type) const noexcept {
    return std::any_of(events_.begin(), events_.end(),
                       [type](const hal::WindowEvent& event) { return event.type == type; });
}

std::size_t FakeWindow::queued_events() const noexcept {
    return events_.size();
}

std::size_t FakeWindow::dropped_events() const noexcept {
    return dropped_events_;
}

std::size_t FakeWindow::redraw_requests() const noexcept {
    return redraw_requests_;
}

bool FakeWindow::visible() const noexcept {
    return visible_;
}

bool FakeWindow::closed() const noexcept {
    return closed_;
}

void FakeWindow::push_metrics_event(hal::WindowEventType type) {
    if (events_.size() >= kMaxFakeWindowEvents) {
        events_.pop_front();
        ++dropped_events_;
    }
    events_.push_back(hal::WindowEvent{type, metrics_});
}

void FakeWindow::update_physical_size() {
    const double scale = static_cast<double>(metrics_.dpi_scale);
    metrics_.physical_width = to_physical(metrics_.logical_width, scale);
    metrics_.physical_height = to_physical(metrics_.logical_height, scale);
}

} // namespace tessera::harness::testing
