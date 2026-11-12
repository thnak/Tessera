// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "win32_input.hpp"
#include "window_accessibility_win.hpp"
#include "window_event_queue.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

namespace tessera::hal {
namespace {

using detail::EventQueue;

constexpr wchar_t kWindowClassName[] = L"Tessera.Hal.Window";
constexpr UINT kDefaultDpi = 96;
// CREATE_WAITABLE_TIMER_HIGH_RESOLUTION is missing from older SDK headers.
constexpr DWORD kHighResolutionTimer = 0x00000002;
constexpr DWORD kTimerBoundPaddingMs = 20;

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                     static_cast<int>(text.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        // Titles are display-only; fall back to the process code page instead of dropping them.
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()),
                                     nullptr, 0);
    }
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

UINT window_dpi(HWND window) {
    const UINT dpi = GetDpiForWindow(window);
    return dpi == 0 ? kDefaultDpi : dpi;
}

float dpi_scale(UINT dpi) {
    return static_cast<float>(dpi) / static_cast<float>(kDefaultDpi);
}

std::uint32_t logical_from_physical(std::uint32_t physical, UINT dpi) {
    const double scaled =
        static_cast<double>(physical) * static_cast<double>(kDefaultDpi) / static_cast<double>(dpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

std::uint32_t physical_from_logical(std::uint32_t logical, UINT dpi) {
    const double scaled =
        static_cast<double>(logical) * static_cast<double>(dpi) / static_cast<double>(kDefaultDpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

class Win32Window final : public Window {
public:
    Win32Window() = default;

    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const WindowDesc& desc);

    [[nodiscard]] WindowMetrics metrics() const override;
    [[nodiscard]] double display_refresh_hz() const noexcept override;
    [[nodiscard]] NativeWindowHandle native_handle() const override {
        return {nullptr, hwnd_};
    }
    [[nodiscard]] AccessibilityHostHandle accessibility_host_handle() const override {
        return {hwnd_};
    }
    std::size_t poll_events(std::span<WindowEvent> events) override;
    bool wait_for_events(double timeout_seconds) override;
    void show() override;
    void hide() override;
    void request_redraw() override;
    void close() override;
    void set_input_queue(ui::InputQueue* queue) override {
        input_.set_queue(queue);
    }
    void set_accessibility_root(AccessibilityRoot root) override {
        accessibility_root_ = root;
    }

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void enqueue(WindowEventType type);
    void request_close();
    void pump_messages();
    [[nodiscard]] bool has_pending_events() const;
    // Lazily created on the owning thread; null when the OS has no high-resolution timers.
    [[nodiscard]] HANDLE wait_timer();

    HWND hwnd_ = nullptr;
    HANDLE wait_timer_ = nullptr;
    bool wait_timer_failed_ = false;
    DWORD owner_thread_ = GetCurrentThreadId();
    EventQueue events_;
    Win32InputTranslator input_;
    AccessibilityRoot accessibility_root_{};
    bool focused_ = false;
    bool minimized_ = false;
    bool initializing_ = true;
    bool applying_dpi_rect_ = false;
    bool close_posted_ = false;
    bool close_requested_ = false;
    bool close_delivered_ = false;
};

LRESULT CALLBACK Win32Window::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Win32Window* instance = nullptr;
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        instance = static_cast<Win32Window*>(create->lpCreateParams);
        if (instance != nullptr) {
            instance->hwnd_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(instance));
        }
    } else {
        instance = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (instance == nullptr) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return instance->handle_message(window, message, wparam, lparam);
}

bool Win32Window::create(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return false;
    }
    // The process must be per-monitor DPI aware before the first window exists; a host may have
    // already selected awareness, so a failure here is not fatal.
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW window_class{};
    window_class.cbSize = static_cast<UINT>(sizeof(window_class));
    window_class.style = CS_DBLCLKS;
    window_class.lpfnWndProc = &Win32Window::window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    window_class.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&window_class) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const UINT dpi = GetDpiForSystem();
    const std::uint32_t client_width = physical_from_logical(desc.width, dpi);
    const std::uint32_t client_height = physical_from_logical(desc.height, dpi);
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD ex_style = 0;
    RECT rect{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
    if (!AdjustWindowRectExForDpi(&rect, style, FALSE, ex_style, dpi)) {
        rect = RECT{0, 0, static_cast<LONG>(client_width), static_cast<LONG>(client_height)};
    }

    const std::wstring title = widen(desc.title);
    hwnd_ = CreateWindowExW(ex_style, kWindowClassName, title.empty() ? L"Tessera" : title.c_str(),
                            style, CW_USEDEFAULT, CW_USEDEFAULT,
                            static_cast<int>(rect.right - rect.left),
                            static_cast<int>(rect.bottom - rect.top), nullptr, nullptr,
                            GetModuleHandleW(nullptr), this);
    if (hwnd_ == nullptr) {
        return false;
    }

    // The initial monitor may not match the system DPI used above; normalize the client area so
    // WindowDesc stays logical pixels on the monitor the window actually landed on.
    const UINT actual_dpi = window_dpi(hwnd_);
    if (actual_dpi != dpi) {
        RECT target{0, 0, static_cast<LONG>(physical_from_logical(desc.width, actual_dpi)),
                    static_cast<LONG>(physical_from_logical(desc.height, actual_dpi))};
        if (AdjustWindowRectExForDpi(&target, style, FALSE, ex_style, actual_dpi) != FALSE) {
            SetWindowPos(hwnd_, nullptr, 0, 0, static_cast<int>(target.right - target.left),
                         static_cast<int>(target.bottom - target.top),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    if (desc.visible) {
        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
    }
    // Creation-time messages are state, not events: the runtime reads the initial metrics.
    initializing_ = false;
    events_.clear();
    return true;
}

Win32Window::~Win32Window() {
    if (wait_timer_ != nullptr) {
        CloseHandle(wait_timer_);
        wait_timer_ = nullptr;
    }
    if (hwnd_ == nullptr) {
        return;
    }
    // DestroyWindow is only valid on the owning thread. A foreign-thread destructor detaches and
    // leaks the OS window rather than racing the owning thread's message loop.
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
    if (GetCurrentThreadId() == owner_thread_) {
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

WindowMetrics Win32Window::metrics() const {
    WindowMetrics metrics;
    if (hwnd_ == nullptr) {
        metrics.focused = false;
        return metrics;
    }

    RECT client{};
    if (GetClientRect(hwnd_, &client) == FALSE) {
        return metrics;
    }
    const UINT dpi = window_dpi(hwnd_);
    metrics.physical_width =
        static_cast<std::uint32_t>(std::max<LONG>(client.right - client.left, 0));
    metrics.physical_height =
        static_cast<std::uint32_t>(std::max<LONG>(client.bottom - client.top, 0));
    metrics.logical_width = logical_from_physical(metrics.physical_width, dpi);
    metrics.logical_height = logical_from_physical(metrics.physical_height, dpi);
    metrics.dpi_scale = dpi_scale(dpi);
    metrics.focused = focused_;
    metrics.minimized = IsIconic(hwnd_) != FALSE;
    return metrics;
}

double Win32Window::display_refresh_hz() const noexcept {
    if (hwnd_ == nullptr) {
        return kUnknownRefreshHz;
    }
    // The monitor the window is on, not the primary one: a multi-monitor desktop can mix rates and
    // the present cap must follow the display the user actually sees. The display-mode API is
    // chosen over DwmGetCompositionTimingInfo because the latter describes the compositor's timing
    // for the whole desktop (and needs an active composition), while EnumDisplaySettingsExW reports
    // this monitor's current mode.
    const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (monitor == nullptr) {
        return kUnknownRefreshHz;
    }
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info) == FALSE) {
        return kUnknownRefreshHz;
    }
    DEVMODEW mode{};
    mode.dmSize = static_cast<WORD>(sizeof(mode));
    if (EnumDisplaySettingsExW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode, 0) == FALSE) {
        return kUnknownRefreshHz;
    }
    // DEVMODE documents 0 ("hardware default") and 1 ("default") as frequency sentinels, never as
    // a real rate; the pure sanitizer would accept 1 Hz, so reject both here.
    if (mode.dmDisplayFrequency <= 1) {
        return kUnknownRefreshHz;
    }
    return sanitize_display_refresh_hz(static_cast<double>(mode.dmDisplayFrequency));
}

void Win32Window::enqueue(WindowEventType type) {
    if (initializing_) {
        return;
    }
    WindowEvent event;
    event.type = type;
    event.metrics = metrics();
    events_.push(event);
}

void Win32Window::request_close() {
    if (close_requested_) {
        return;
    }
    close_requested_ = true;
    close_delivered_ = false;
    enqueue(WindowEventType::Close);
}

void Win32Window::pump_messages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        if (message.message == WM_QUIT) {
            // Thread shutdown has no HWND; surface it as the normalized shutdown event.
            request_close();
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool Win32Window::has_pending_events() const {
    return events_.count() > 0 || (close_requested_ && !close_delivered_) || input_.pending();
}

std::size_t Win32Window::poll_events(std::span<WindowEvent> events) {
    const std::size_t copied = events_.copy_to(events);
    for (std::size_t i = 0; i < copied; ++i) {
        if (events[i].type == WindowEventType::Close) {
            close_delivered_ = true;
        }
    }
    return copied;
}

HANDLE Win32Window::wait_timer() {
    if (wait_timer_ == nullptr && !wait_timer_failed_) {
        // The default sleep granularity is the 15.6 ms system tick, which turns a 16.7 ms frame
        // deadline into a ~31 ms wait. A high-resolution timer (Windows 10 1803+) wakes within
        // about a millisecond without changing the global timer resolution.
        wait_timer_ =
            CreateWaitableTimerExW(nullptr, nullptr, kHighResolutionTimer, TIMER_ALL_ACCESS);
        wait_timer_failed_ = wait_timer_ == nullptr;
    }
    return wait_timer_;
}

bool Win32Window::wait_for_events(double timeout_seconds) {
    // Message pumping and the event queue belong to the creating thread; a foreign-thread wait
    // cannot observe them safely.
    if (GetCurrentThreadId() != owner_thread_) {
        return false;
    }
    const double timeout =
        std::isfinite(timeout_seconds) && timeout_seconds > 0.0 ? timeout_seconds : 0.0;
    const double deadline = steady_now_seconds() + timeout;

    for (;;) {
        pump_messages();
        if (has_pending_events()) {
            return true;
        }
        const double remaining = seconds_until(steady_now_seconds(), deadline);
        if (remaining <= 0.0) {
            return false;
        }
        // MsgWaitForMultipleObjectsEx takes whole milliseconds. Round up so a sub-millisecond
        // remainder still parks once instead of spinning, and clamp so the wait stays bounded.
        const double wait_milliseconds =
            std::min(std::ceil(remaining * 1000.0),
                     static_cast<double>(std::numeric_limits<DWORD>::max() - 1));
        const DWORD wait_ms = static_cast<DWORD>(wait_milliseconds);

        // Park on the high-resolution timer plus the message queue. The millisecond timeout stays
        // as a bound in case the timer never fires (it is padded so the timer normally wins).
        HANDLE timer = wait_timer();
        bool timer_armed = false;
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -std::max<LONGLONG>(1, static_cast<LONGLONG>(remaining * 1.0e7));
            timer_armed = SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
        }
        const DWORD result =
            timer_armed ? MsgWaitForMultipleObjectsEx(1, &timer, wait_ms + kTimerBoundPaddingMs,
                                                      QS_ALLINPUT, 0)
                        : MsgWaitForMultipleObjectsEx(0, nullptr, wait_ms, QS_ALLINPUT, 0);
        if (result == WAIT_FAILED) {
            // A broken message queue must not spin; idle out the remaining budget once.
            wait_until_seconds(deadline);
            return has_pending_events();
        }
        if (result == WAIT_TIMEOUT || (timer_armed && result == WAIT_OBJECT_0)) {
            pump_messages();
            return has_pending_events();
        }
        // Otherwise input is available; loop to drain and re-check for events.
    }
}

void Win32Window::show() {
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_SHOW);
    }
}

void Win32Window::hide() {
    if (hwnd_ != nullptr) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void Win32Window::request_redraw() {
    if (hwnd_ != nullptr) {
        // InvalidateRect coalesces repeated requests until the next WM_PAINT.
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void Win32Window::close() {
    if (hwnd_ == nullptr || close_posted_) {
        return;
    }
    close_posted_ = true;
    PostMessageW(hwnd_, WM_CLOSE, 0, 0);
}

LRESULT Win32Window::handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    LRESULT input_result = 0;
    if (!initializing_ && input_.handle(window, message, wparam, lparam, input_result)) {
        return input_result;
    }
    switch (message) {
    case WM_SIZE: {
        if (wparam == SIZE_MINIMIZED) {
            if (!minimized_) {
                minimized_ = true;
                enqueue(WindowEventType::Minimized);
            }
            return 0;
        }
        if (minimized_) {
            minimized_ = false;
            enqueue(WindowEventType::Restored);
        }
        if (!applying_dpi_rect_) {
            enqueue(WindowEventType::Resize);
        }
        return 0;
    }
    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            applying_dpi_rect_ = true;
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         static_cast<int>(suggested->right - suggested->left),
                         static_cast<int>(suggested->bottom - suggested->top),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            applying_dpi_rect_ = false;
        }
        enqueue(WindowEventType::DpiChange);
        return 0;
    }
    case WM_SETFOCUS:
        focused_ = true;
        enqueue(WindowEventType::Focus);
        return 0;
    case WM_KILLFOCUS:
        focused_ = false;
        enqueue(WindowEventType::Blur);
        return 0;
    case WM_SETTINGCHANGE:
        // Only the animation toggle is surfaced; other settings broadcasts stay default-handled.
        if (wparam == SPI_SETCLIENTAREAANIMATION) {
            enqueue(WindowEventType::SystemSettingsChanged);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    case WM_CLOSE:
        // Graceful shutdown is a normalized event; the owner destroys the window.
        request_close();
        return 0;
    case WM_GETOBJECT: {
        // UI Automation asks for the root provider. The answer is a pointer the bridge thread
        // published before it became visible; the window thread never waits on COM. When no root
        // is ready (starting, failed, stopped, or a non-UIA object id) DefWindowProc keeps the
        // default MSAA bridge instead of answering with an empty provider.
        const auto answered = detail::answer_accessibility_get_object(
            window, static_cast<std::uintptr_t>(wparam), static_cast<std::intptr_t>(lparam),
            accessibility_root_);
        if (answered.has_value()) {
            return static_cast<LRESULT>(*answered);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    case WM_SHOWWINDOW:
        if (wparam == FALSE) {
            focused_ = false;
        } else if (!initializing_) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        // The hook must not survive the window: its context may already be gone.
        accessibility_root_ = {};
        hwnd_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

bool Window::wait_for_events(double timeout_seconds) {
    // Default for platform windows without an OS event source: idle for the bounded timeout and
    // report no event so a polling caller still throttles instead of spinning.
    if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
        wait_until_seconds(steady_now_seconds() + timeout_seconds);
    }
    return false;
}

std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    auto window = std::make_unique<Win32Window>();
    if (!window->create(desc)) {
        return nullptr;
    }
    return window;
}

} // namespace tessera::hal
