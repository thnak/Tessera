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

#include "tessera/hal/window.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <windows.h>

namespace tessera::hal {
namespace {

constexpr wchar_t kWindowClassName[] = L"Tessera.Hal.Window";
constexpr UINT kDefaultDpi = 96;
constexpr std::size_t kMaxQueuedEvents = 64;

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

// Small fixed-capacity ring. OS callbacks only record, so an overflow drops the oldest event
// instead of allocating; repeated resizes coalesce into the newest metrics.
class EventQueue {
public:
    void push(const WindowEvent& event) {
        if (event.type == WindowEventType::Resize && count_ > 0) {
            WindowEvent& newest = at(count_ - 1);
            if (newest.type == WindowEventType::Resize) {
                newest = event;
                return;
            }
        }
        if (count_ == events_.size()) {
            head_ = (head_ + 1) % events_.size();
            --count_;
        }
        events_[(head_ + count_) % events_.size()] = event;
        ++count_;
    }

    std::size_t copy_to(std::span<WindowEvent> events) {
        const std::size_t copied = std::min(events.size(), count_);
        for (std::size_t i = 0; i < copied; ++i) {
            events[i] = at(i);
        }
        head_ = (head_ + copied) % events_.size();
        count_ -= copied;
        return copied;
    }

    void clear() {
        head_ = 0;
        count_ = 0;
    }

private:
    WindowEvent& at(std::size_t index) {
        return events_[(head_ + index) % events_.size()];
    }

    std::array<WindowEvent, kMaxQueuedEvents> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

class Win32Window final : public Window {
public:
    Win32Window() = default;

    ~Win32Window() override;

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool create(const WindowDesc& desc);

    [[nodiscard]] WindowMetrics metrics() const override;
    std::size_t poll_events(std::span<WindowEvent> events) override;
    void show() override;
    void hide() override;
    void request_redraw() override;
    void close() override;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    void enqueue(WindowEventType type);

    HWND hwnd_ = nullptr;
    DWORD owner_thread_ = GetCurrentThreadId();
    EventQueue events_;
    bool focused_ = false;
    bool minimized_ = false;
    bool initializing_ = true;
    bool applying_dpi_rect_ = false;
    bool close_posted_ = false;
    bool close_requested_ = false;
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

void Win32Window::enqueue(WindowEventType type) {
    if (initializing_) {
        return;
    }
    WindowEvent event;
    event.type = type;
    event.metrics = metrics();
    events_.push(event);
}

std::size_t Win32Window::poll_events(std::span<WindowEvent> events) {
    return events_.copy_to(events);
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
    case WM_CLOSE:
        // Graceful shutdown is a normalized event; the owner destroys the window.
        if (!close_requested_) {
            close_requested_ = true;
            enqueue(WindowEventType::Close);
        }
        return 0;
    case WM_SHOWWINDOW:
        if (wparam == FALSE) {
            focused_ = false;
        } else if (!initializing_) {
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_NCDESTROY:
        hwnd_ = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        return DefWindowProcW(window, message, wparam, lparam);
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

} // namespace

std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    auto window = std::make_unique<Win32Window>();
    if (!window->create(desc)) {
        return nullptr;
    }
    return window;
}

} // namespace tessera::hal
