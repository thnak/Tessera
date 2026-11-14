// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "tessera/hal/window.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <windows.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_win32: " << message << "\n";
    }
    return condition;
}

bool approx(float value, float expected, float tolerance = 1.0f) {
    return value >= expected - tolerance && value <= expected + tolerance;
}

void pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

HWND find_window(std::string_view title) {
    return FindWindowA(nullptr, std::string(title).c_str());
}

// Sets the outer window size so the client area becomes `client_width` x `client_height` physical
// pixels at the window's current DPI.
bool resize_client(HWND window, std::uint32_t client_width, std::uint32_t client_height) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));
    RECT rect{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
    if (AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, GetDpiForWindow(window)) == FALSE) {
        return false;
    }
    return SetWindowPos(window, nullptr, 0, 0, static_cast<int>(rect.right - rect.left),
                        static_cast<int>(rect.bottom - rect.top),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
}

bool contains_event(const std::array<tessera::hal::WindowEvent, 32>& events, std::size_t count,
                    tessera::hal::WindowEventType type,
                    const tessera::hal::WindowEvent** found = nullptr) {
    for (std::size_t i = 0; i < count; ++i) {
        if (events[i].type == type) {
            if (found != nullptr) {
                *found = &events[i];
            }
            return true;
        }
    }
    return false;
}

} // namespace

int main() {
    using tessera::hal::WindowEvent;
    using tessera::hal::WindowEventType;
    using tessera::hal::WindowMetrics;

    const std::string title = "Tessera Win32 window test " + std::to_string(GetCurrentProcessId());

    tessera::hal::WindowDesc desc;
    desc.title = title;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;

    auto window = tessera::hal::create_window(desc);
    if (!expect(window != nullptr, "create_window returns a window")) {
        return 1;
    }

    const HWND hwnd = find_window(title);
    if (!expect(hwnd != nullptr, "native window exists")) {
        return 2;
    }

    const WindowMetrics initial = window->metrics();
    if (!expect(initial.physical_width > 0 && initial.physical_height > 0,
                "initial physical size is non-zero") ||
        !expect(approx(static_cast<float>(initial.logical_width), 320.0f),
                "initial logical width follows WindowDesc") ||
        !expect(approx(static_cast<float>(initial.logical_height), 200.0f),
                "initial logical height follows WindowDesc") ||
        !expect(initial.dpi_scale > 0.0f, "initial dpi scale is positive") ||
        !expect(!initial.minimized, "hidden window is not minimized")) {
        return 3;
    }

    std::array<WindowEvent, 32> events{};
    if (!expect(window->poll_events(events) == 0, "creation records no events")) {
        return 4;
    }

    // The display-refresh query is a per-monitor OS read: it reports either a real rate inside the
    // documented sane bounds or the documented "unknown" value (a virtual/RDP session may show no
    // mode), never a throw and never a guessed rate.
    const double refresh_hz = window->display_refresh_hz();
    if (!expect(refresh_hz == tessera::hal::kUnknownRefreshHz ||
                    (refresh_hz >= tessera::hal::kMinDisplayRefreshHz &&
                     refresh_hz <= tessera::hal::kMaxDisplayRefreshHz),
                "display refresh is unknown or a sane rate")) {
        return 12;
    }
    std::cout << "window_win32: display refresh " << refresh_hz << " Hz\n";

    // Resizing the hidden window must produce one coalesced Resize carrying the new metrics.
    if (!expect(resize_client(hwnd, 480, 320), "SetWindowPos resize succeeds")) {
        return 5;
    }
    pump_messages();

    const std::size_t resize_count = window->poll_events(events);
    const WindowEvent* resize_event = nullptr;
    if (!expect(contains_event(events, resize_count, WindowEventType::Resize, &resize_event),
                "resize produces a Resize event")) {
        return 6;
    }
    const WindowMetrics resized = window->metrics();
    if (!expect(resized.physical_width == 480 && resized.physical_height == 320,
                "metrics follow the new client size") ||
        !expect(resize_event->metrics.physical_width == resized.physical_width &&
                    resize_event->metrics.physical_height == resized.physical_height,
                "Resize event carries current metrics") ||
        !expect(approx(resized.dpi_scale, initial.dpi_scale), "resize keeps the dpi scale")) {
        return 7;
    }

    // Requesting a redraw is coalesced by InvalidateRect and must not emit a normalized event.
    window->request_redraw();
    window->request_redraw();
    pump_messages();
    if (!expect(window->poll_events(events) == 0, "redraw requests emit no window events")) {
        return 8;
    }

    // Full-screen round trip (#232): entering covers the monitor and reports the mode, leaving
    // restores the client size the window had. A real window manager/compositor is not needed: the
    // backend itself applies and reverts its own placement.
    {
        if (!expect(window->supports_fullscreen(), "the Win32 backend supports full-screen")) {
            return 13;
        }
        const WindowMetrics before = window->metrics();
        window->set_fullscreen(true);
        pump_messages();
        (void)window->poll_events(events);
        const WindowMetrics full = window->metrics();
        if (!expect(full.fullscreen, "entering reports the full-screen mode") ||
            !expect(full.physical_width >= before.physical_width,
                    "the full-screen client area is at least the windowed one") ||
            !expect(full.logical_width >= before.logical_width,
                    "the full-screen logical width is at least the windowed one")) {
            return 14;
        }
        window->set_fullscreen(false);
        pump_messages();
        (void)window->poll_events(events);
        const WindowMetrics restored = window->metrics();
        if (!expect(!restored.fullscreen, "leaving reports the windowed mode again") ||
            !expect(restored.physical_width == before.physical_width,
                    "leaving restores the pre-full-screen client width") ||
            !expect(restored.physical_height == before.physical_height,
                    "leaving restores the pre-full-screen client height")) {
            return 15;
        }
        // A repeated request is harmless and leaves the mode as it was.
        window->set_fullscreen(true);
        window->set_fullscreen(true);
        pump_messages();
        (void)window->poll_events(events);
        if (!expect(window->metrics().fullscreen, "a repeated enter stays full-screen")) {
            return 16;
        }
        window->set_fullscreen(false);
        pump_messages();
        (void)window->poll_events(events);
    }

    // Caption regions (#232, window-chrome-v1.md section 8.1): publishing them is reported through
    // the metrics, and an empty list returns the window to the system-frame answer.
    {
        const std::array<tessera::hal::CaptionRegion, 2> regions{
            tessera::hal::CaptionRegion{tessera::hal::WindowRect{0, 0, 32, 32},
                                        tessera::hal::CaptionRegionKind::SystemMenu},
            tessera::hal::CaptionRegion{tessera::hal::WindowRect{260, 0, 46, 32},
                                        tessera::hal::CaptionRegionKind::Close},
        };
        window->set_caption_regions(regions);
        pump_messages();
        const WindowMetrics custom = window->metrics();
        if (!expect(custom.caption_region_count == 2, "the caption regions are published") ||
            !expect(custom.caption_regions[1].kind == tessera::hal::CaptionRegionKind::Close,
                    "the published kinds are kept")) {
            return 17;
        }
        window->set_caption_regions({});
        pump_messages();
        if (!expect(window->metrics().caption_region_count == 0,
                    "an empty list returns the window to the system frame")) {
            return 18;
        }
    }

    window->hide();
    window->show();
    pump_messages();
    (void)window->poll_events(events);

    window->close();
    pump_messages();
    const std::size_t close_count = window->poll_events(events);
    if (!expect(contains_event(events, close_count, WindowEventType::Close),
                "close produces a Close event")) {
        return 9;
    }

    // Closing twice is harmless.
    window->close();
    pump_messages();
    if (!expect(window->poll_events(events) == 0, "second close is a no-op")) {
        return 10;
    }

    window.reset();

    // Invalid descriptors are rejected without creating a native window.
    tessera::hal::WindowDesc invalid = desc;
    invalid.width = 0;
    if (!expect(tessera::hal::create_window(invalid) == nullptr, "zero width is rejected")) {
        return 11;
    }

    std::cout << "window_win32: ok\n";
    return 0;
}
