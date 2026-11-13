// SPDX-License-Identifier: MIT

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"
#include "window_event_queue.hpp"
#include "x11_backdrop.hpp"
#include "x11_input.hpp"
#include "x11_xi2.hpp"
#if defined(TESSERA_X11_RANDR)
#include "x11_randr.hpp"
#endif

#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace tessera::hal {
namespace {

using detail::EventQueue;

constexpr int kDefaultDpi = 96;
// Xft.dpi is a user-controlled Xresource. Clamp obviously broken values so a typo cannot turn
// every logical size into zero or a multi-thousand-pixel window.
constexpr int kMinDpi = 48;
constexpr int kMaxDpi = 768;

float dpi_scale(int dpi) {
    return static_cast<float>(dpi) / static_cast<float>(kDefaultDpi);
}

std::uint32_t logical_from_physical(std::uint32_t physical, int dpi) {
    const double scaled =
        static_cast<double>(physical) * static_cast<double>(kDefaultDpi) / static_cast<double>(dpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

std::uint32_t physical_from_logical(std::uint32_t logical, int dpi) {
    const double scaled =
        static_cast<double>(logical) * static_cast<double>(dpi) / static_cast<double>(kDefaultDpi);
    return static_cast<std::uint32_t>(std::lround(scaled));
}

// X11 has no per-window DPI query; the desktop publishes a global Xft.dpi Xresource. Fall back
// to 96 when the resource server is absent (bare Xvfb, minimal window managers).
int query_xft_dpi(Display* display) {
    const char* resources = XResourceManagerString(display);
    if (resources == nullptr || resources[0] == '\0') {
        return kDefaultDpi;
    }
    XrmDatabase database = XrmGetStringDatabase(resources);
    if (database == nullptr) {
        return kDefaultDpi;
    }

    int dpi = kDefaultDpi;
    char* type = nullptr;
    XrmValue value{};
    if (XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) != 0 &&
        value.addr != nullptr) {
        const char* text = static_cast<const char*>(value.addr);
        char* end = nullptr;
        const double parsed = std::strtod(text, &end);
        if (end != text && std::isfinite(parsed) && parsed > 0.0) {
            dpi = std::clamp(static_cast<int>(std::lround(parsed)), kMinDpi, kMaxDpi);
        }
    }
    XrmDestroyDatabase(database);
    return dpi;
}

class X11Window final : public Window {
public:
    X11Window() = default;

    ~X11Window() override;

    X11Window(const X11Window&) = delete;
    X11Window& operator=(const X11Window&) = delete;

    bool create(const WindowDesc& desc);

    [[nodiscard]] WindowMetrics metrics() const override;
    [[nodiscard]] double display_refresh_hz() const noexcept override;
    [[nodiscard]] NativeWindowHandle native_handle() const override {
        return {display_, reinterpret_cast<void*>(window_)};
    }
    std::size_t poll_events(std::span<WindowEvent> events) override;
    bool wait_for_events(double timeout_seconds) override;
    void set_input_queue(ui::InputQueue* queue) override {
        input_.set_queue(queue);
        xi2_.set_queue(queue);
        // The XI2 pre-edit machine and the key path share one owner per commit: the gate drops the
        // key event carrying text the composition already committed as an EditText event.
        input_.set_ime_commit_gate({&X11Xi2Pump::consume_commit_thunk, static_cast<void*>(&xi2_)});
    }
    void show() override;
    void hide() override;
    void request_redraw() override;
    void close() override;

private:
    void handle_event(XEvent& event, bool auto_repeat = false, bool filtered = false);
    void enqueue(WindowEventType type);
    void request_close();
    void pump(bool discard);
    void open_input_method();
    void close_input_method();
    [[nodiscard]] bool has_pending_events() const;

    Display* display_ = nullptr;
    ::Window window_ = 0;
    ::Window root_ = 0;
    int screen_ = 0;
    int dpi_ = kDefaultDpi;
    int last_width_ = 0;
    int last_height_ = 0;
    Atom wm_protocols_ = 0;
    Atom wm_delete_window_ = 0;
    std::thread::id owner_thread_ = std::this_thread::get_id();
    EventQueue events_;
    X11InputTranslator input_;
    X11Xi2Pump xi2_;
    XIM input_method_ = nullptr;
    XIC input_context_ = nullptr;
    // The OS backdrop this window resolved at creation (glass-v1.md section 6). The engine reads it
    // only through WindowMetrics; `backdrop_setup_` owns the ARGB colormap of a translucent window.
    detail::X11BackdropSetup backdrop_setup_;
    BackdropResolution backdrop_{};
    bool initializing_ = true;
    bool mapped_ = false;
    bool focused_ = false;
    bool redraw_pending_ = false;
    bool close_posted_ = false;
    bool close_requested_ = false;
    bool close_delivered_ = false;
};

bool X11Window::create(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return false;
    }
    display_ = XOpenDisplay(nullptr);
    if (display_ == nullptr) {
        return false;
    }
    screen_ = DefaultScreen(display_);
    root_ = RootWindow(display_, screen_);
    dpi_ = query_xft_dpi(display_);

    const std::uint32_t width = physical_from_logical(desc.width, dpi_);
    const std::uint32_t height = physical_from_logical(desc.height, dpi_);

    // A translucent window needs a 32-bit ARGB visual so the swapchain's per-pixel alpha reaches
    // the compositor; the opaque path keeps the parent visual byte-identical. The visual and depth
    // are XCreateWindow inputs, so the capability query happens before creation.
    const bool wants_translucent = desc.transparent || desc.backdrop != WindowBackdrop::None;
    backdrop_setup_ = detail::prepare_x11_backdrop(display_, screen_, wants_translucent);

    XSetWindowAttributes attributes{};
    attributes.background_pixel = BlackPixel(display_, screen_);
    // StructureNotify carries ConfigureNotify/DestroyNotify; Exposure carries the redraw wake;
    // FocusChange is normalized to Focus/Blur; key, button and motion events feed the input queue.
    attributes.event_mask = StructureNotifyMask | ExposureMask | FocusChangeMask | KeyPressMask |
                            KeyReleaseMask | ButtonPressMask | ButtonReleaseMask |
                            PointerMotionMask;
    unsigned long attribute_mask = CWBackPixel | CWEventMask;
    detail::configure_x11_window_attributes(backdrop_setup_, attributes, attribute_mask);

    window_ =
        XCreateWindow(display_, root_, 0, 0, width, height, 0, backdrop_setup_.depth, InputOutput,
                      backdrop_setup_.visual != nullptr ? backdrop_setup_.visual : CopyFromParent,
                      attribute_mask, &attributes);
    if (window_ == 0) {
        detail::release_x11_backdrop(display_, backdrop_setup_);
        XCloseDisplay(display_);
        display_ = nullptr;
        return false;
    }

    backdrop_ = detail::apply_x11_backdrop(display_, window_, desc.backdrop, desc.transparent,
                                           backdrop_setup_.capabilities);

    wm_protocols_ = XInternAtom(display_, "WM_PROTOCOLS", False);
    wm_delete_window_ = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    if (wm_protocols_ != None && wm_delete_window_ != None) {
        XSetWMProtocols(display_, window_, &wm_delete_window_, 1);
    }
    XStoreName(display_, window_, desc.title.c_str());
    // XI2 is initialized before the input method opens: an active XI2 pump is what lets the window
    // ask for client-side pre-edit, and a missing XInput2 leaves the core path unchanged.
    (void)xi2_.initialize(display_, window_);
    open_input_method();
    last_width_ = static_cast<int>(width);
    last_height_ = static_cast<int>(height);

    if (desc.visible) {
        XMapWindow(display_, window_);
        mapped_ = true;
    }
    XFlush(display_);

    // Creation-time map/configure notifications are state, not events: the runtime reads the
    // initial metrics. Anything already queued is drained and discarded.
    pump(true);
    initializing_ = false;
    events_.clear();
    redraw_pending_ = false;
    return true;
}

X11Window::~X11Window() {
    if (display_ == nullptr) {
        return;
    }
    // Xlib connections are not safe to use from a foreign thread. A foreign-thread destructor
    // detaches and leaks the connection/window rather than racing the owning thread's event loop.
    if (std::this_thread::get_id() != owner_thread_) {
        display_ = nullptr;
        window_ = 0;
        return;
    }
    close_input_method();
    xi2_.shutdown();
    if (window_ != 0) {
        XDestroyWindow(display_, window_);
        window_ = 0;
    }
    detail::release_x11_backdrop(display_, backdrop_setup_);
    XFlush(display_);
    XCloseDisplay(display_);
    display_ = nullptr;
}

WindowMetrics X11Window::metrics() const {
    WindowMetrics metrics;
    if (display_ == nullptr || window_ == 0) {
        metrics.focused = false;
        return metrics;
    }

    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display_, window_, &attributes) == 0) {
        metrics.focused = false;
        return metrics;
    }
    metrics.physical_width = static_cast<std::uint32_t>(std::max(attributes.width, 0));
    metrics.physical_height = static_cast<std::uint32_t>(std::max(attributes.height, 0));
    metrics.logical_width = logical_from_physical(metrics.physical_width, dpi_);
    metrics.logical_height = logical_from_physical(metrics.physical_height, dpi_);
    metrics.dpi_scale = dpi_scale(dpi_);
    metrics.focused = focused_;
    metrics.backdrop = backdrop_.material;
    metrics.transparent = backdrop_.transparent;
    metrics.backdrop_fallback = backdrop_.fallback;
    // X11 has no portable occlusion/minimize signal without a window manager (WM_STATE and
    // _NET_WM_STATE are WM-specific); this backend reports both as false.
    return metrics;
}

double X11Window::display_refresh_hz() const noexcept {
#if defined(TESSERA_X11_RANDR)
    if (display_ == nullptr || window_ == 0 || std::this_thread::get_id() != owner_thread_) {
        return kUnknownRefreshHz;
    }
    return detail::query_x11_display_refresh_hz(display_, window_, root_);
#else
    // Built without libXrandr: this window has no display-refresh source, so no divisor exists and
    // the present cap keeps its configured value.
    return kUnknownRefreshHz;
#endif
}

void X11Window::enqueue(WindowEventType type) {
    if (initializing_) {
        return;
    }
    WindowEvent event;
    event.type = type;
    event.metrics = metrics();
    events_.push(event);
}

void X11Window::request_close() {
    if (close_requested_) {
        return;
    }
    close_requested_ = true;
    close_delivered_ = false;
    enqueue(WindowEventType::Close);
}

void X11Window::handle_event(XEvent& event, bool auto_repeat, bool filtered) {
    // XI2 device events (smooth scroll, touch, pen) are consumed here. Selecting them as XI2
    // events is what stops the server from also delivering the core event for the same device.
    if (xi2_.handle(event, dpi_scale(dpi_))) {
        return;
    }
    // Input events (and the input half of focus changes) go to the queue; focus still falls
    // through to the window's own Focus/Blur state below.
    if (input_.handle(event, dpi_scale(dpi_), auto_repeat, filtered)) {
        return;
    }
    switch (event.type) {
    case ConfigureNotify: {
        const XConfigureEvent& configure = event.xconfigure;
        if (configure.width != last_width_ || configure.height != last_height_) {
            last_width_ = configure.width;
            last_height_ = configure.height;
            enqueue(WindowEventType::Resize);
        }
        return;
    }
    case Expose:
        // A redraw was observed (from request_redraw's XClearArea or a real uncover). The frame
        // is requested through the wait wake; there is no normalized Redraw event type.
        redraw_pending_ = true;
        return;
    case FocusIn:
        focused_ = true;
        if (input_context_ != nullptr) {
            XSetICFocus(input_context_);
        }
        enqueue(WindowEventType::Focus);
        return;
    case FocusOut:
        focused_ = false;
        xi2_.cancel_all();
        if (input_context_ != nullptr) {
            XUnsetICFocus(input_context_);
        }
        enqueue(WindowEventType::Blur);
        return;
    case MappingNotify:
        // A layout or keymap change (setxkbmap, a layout switcher): Xlib keeps its own copy of the
        // keymap, and keys translate through the old one until it is refreshed.
        XRefreshKeyboardMapping(&event.xmapping);
        return;
    case MapNotify:
        mapped_ = true;
        return;
    case UnmapNotify:
        mapped_ = false;
        focused_ = false;
        redraw_pending_ = false;
        return;
    case ClientMessage:
        if (event.xclient.message_type == wm_protocols_ && wm_delete_window_ != None &&
            static_cast<Atom>(event.xclient.data.l[0]) == wm_delete_window_) {
            request_close();
        }
        return;
    case DestroyNotify:
        if (event.xdestroywindow.window == window_) {
            window_ = 0;
            request_close();
        }
        return;
    default:
        return;
    }
}

void X11Window::open_input_method() {
    // Compose and input methods need a locale that Xlib supports. An application that set its own
    // locale keeps it; one left at "C" gets UTF-8 character handling, the one setting this window
    // needs (LC_CTYPE only: numbers, dates and collation stay untouched).
    const char* current = std::setlocale(LC_CTYPE, nullptr);
    if (current == nullptr || std::string_view(current) == "C" ||
        std::string_view(current) == "POSIX") {
        if (std::setlocale(LC_CTYPE, "C.UTF-8") == nullptr) {
            std::setlocale(LC_CTYPE, "");
        }
    }
    if (XSupportsLocale() == 0) {
        return; // keysym-only text
    }
    XSetLocaleModifiers("");
    input_method_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    if (input_method_ == nullptr) {
        // No input-method server is running: the built-in method still does dead keys and compose.
        XSetLocaleModifiers("@im=none");
        input_method_ = XOpenIM(display_, nullptr, nullptr, nullptr);
    }
    if (input_method_ == nullptr) {
        return;
    }
    // The method composes and commits text through the IC. Client-side pre-edit is requested when
    // the method offers it and the XI2 pump is active, because that is the path that carries the
    // CompositionStart/Update/End events; the fallback IC keeps today's commit-only behavior.
    input_context_ = XCreateIC(input_method_, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                               XNClientWindow, window_, XNFocusWindow, window_, nullptr);
    if (input_context_ == nullptr) {
        XCloseIM(input_method_);
        input_method_ = nullptr;
        return;
    }
    if (xi2_.enabled() && xi2_.input_method_supports_preedit(input_method_)) {
        XIC with_preedit =
            XCreateIC(input_method_, XNInputStyle, XIMPreeditCallbacks | XIMStatusNothing,
                      XNClientWindow, window_, XNFocusWindow, window_, nullptr);
        if (with_preedit != nullptr && xi2_.attach_input_context(input_method_, with_preedit)) {
            XDestroyIC(input_context_);
            input_context_ = with_preedit;
        } else if (with_preedit != nullptr) {
            // The method advertised callbacks but refused them: keep the commit-only context.
            XDestroyIC(with_preedit);
        }
    }
    // The method names the events it needs delivered to filter key input.
    unsigned long filter_events = 0;
    if (XGetICValues(input_context_, XNFilterEvents, &filter_events, nullptr) == nullptr) {
        XWindowAttributes attributes{};
        XGetWindowAttributes(display_, window_, &attributes);
        XSelectInput(display_, window_,
                     attributes.your_event_mask | static_cast<long>(filter_events));
    }
    input_.set_input_context(input_context_);
}

void X11Window::close_input_method() {
    input_.set_input_context(nullptr);
    if (input_context_ != nullptr) {
        XDestroyIC(input_context_);
        input_context_ = nullptr;
    }
    if (input_method_ != nullptr) {
        XCloseIM(input_method_);
        input_method_ = nullptr;
    }
}

void X11Window::pump(bool discard) {
    if (display_ == nullptr) {
        return;
    }
    while (XPending(display_) > 0) {
        XEvent event{};
        XNextEvent(display_, &event);
        // The input method sees every event first (XIM protocol messages included); a filtered
        // key event is still delivered as a key, a filtered anything else is the method's own.
        const bool filtered = input_method_ != nullptr && XFilterEvent(&event, None) != 0;
        if (discard) {
            continue;
        }
        if (filtered && event.type != KeyPress && event.type != KeyRelease) {
            continue;
        }
        bool auto_repeat = false;
        if (event.type == KeyRelease && XPending(display_) > 0) {
            // Auto-repeat arrives as a release immediately followed by a press with the same
            // keycode and timestamp. Drop the release and deliver the press as a repeat.
            XEvent next{};
            XPeekEvent(display_, &next);
            if (next.type == KeyPress &&
                X11InputTranslator::is_auto_repeat_pair(event.xkey, next.xkey)) {
                XNextEvent(display_, &event);
                auto_repeat = true;
            }
        }
        handle_event(event, auto_repeat, filtered);
    }
}

bool X11Window::has_pending_events() const {
    return events_.count() > 0 || (close_requested_ && !close_delivered_) || redraw_pending_ ||
           input_.pending();
}

std::size_t X11Window::poll_events(std::span<WindowEvent> events) {
    pump(false);
    const std::size_t copied = events_.copy_to(events);
    for (std::size_t i = 0; i < copied; ++i) {
        if (events[i].type == WindowEventType::Close) {
            close_delivered_ = true;
        }
    }
    // A redraw wake is consumed by the poll that observes it; the runtime draws after polling.
    redraw_pending_ = false;
    return copied;
}

bool X11Window::wait_for_events(double timeout_seconds) {
    // The Xlib connection and the event queue belong to the creating thread.
    if (std::this_thread::get_id() != owner_thread_ || display_ == nullptr) {
        return false;
    }
    const double timeout =
        std::isfinite(timeout_seconds) && timeout_seconds > 0.0 ? timeout_seconds : 0.0;
    const double deadline = steady_now_seconds() + timeout;

    for (;;) {
        pump(false);
        if (has_pending_events()) {
            return true;
        }
        const double remaining = seconds_until(steady_now_seconds(), deadline);
        if (remaining <= 0.0) {
            return false;
        }
        // poll() takes whole milliseconds. Round up (and never pass 0) so a sub-millisecond
        // remainder parks once instead of spinning, and clamp so the wait stays bounded.
        const double wait_milliseconds = std::max(1.0, std::ceil(remaining * 1000.0));
        const int milliseconds = wait_milliseconds >= static_cast<double>(INT_MAX)
                                     ? INT_MAX
                                     : static_cast<int>(wait_milliseconds);
        pollfd descriptor{};
        descriptor.fd = ConnectionNumber(display_);
        descriptor.events = POLLIN;
        const int result = poll(&descriptor, 1, milliseconds);
        if (result > 0) {
            if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                // A dead connection never becomes readable; idle out the budget once so callers
                // still throttle instead of spinning on a broken display.
                wait_until_seconds(deadline);
                pump(false);
                return has_pending_events();
            }
            continue; // Readable: drain and re-check for normalized events.
        }
        if (result == 0) {
            pump(false);
            return has_pending_events();
        }
        if (errno == EINTR) {
            continue;
        }
        wait_until_seconds(deadline);
        pump(false);
        return has_pending_events();
    }
}

void X11Window::show() {
    if (display_ == nullptr || window_ == 0 || mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    XMapWindow(display_, window_);
    mapped_ = true;
    XFlush(display_);
}

void X11Window::hide() {
    if (display_ == nullptr || window_ == 0 || !mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    XUnmapWindow(display_, window_);
    mapped_ = false;
    focused_ = false;
    redraw_pending_ = false;
    XFlush(display_);
}

void X11Window::request_redraw() {
    if (display_ == nullptr || window_ == 0 || !mapped_ ||
        std::this_thread::get_id() != owner_thread_) {
        // A hidden window has nothing to repaint; mapping produces an Expose wake instead.
        return;
    }
    if (redraw_pending_) {
        return;
    }
    redraw_pending_ = true;
    XClearArea(display_, window_, 0, 0, 0, 0, True);
    XFlush(display_);
}

void X11Window::close() {
    if (display_ == nullptr || window_ == 0 || close_posted_ ||
        std::this_thread::get_id() != owner_thread_) {
        return;
    }
    close_posted_ = true;
    if (wm_protocols_ == 0 || wm_delete_window_ == 0) {
        request_close();
        return;
    }

    // Deliver the same WM_DELETE_WINDOW ClientMessage a window manager would, so graceful close
    // and external close share one code path. The event loops back through handle_event().
    XClientMessageEvent message{};
    message.type = ClientMessage;
    message.display = display_;
    message.window = window_;
    message.message_type = wm_protocols_;
    message.format = 32;
    message.data.l[0] = static_cast<long>(wm_delete_window_);
    message.data.l[1] = CurrentTime;
    XEvent event{};
    event.xclient = message;
    if (XSendEvent(display_, window_, False, NoEventMask, &event) == 0) {
        request_close();
    }
    XFlush(display_);
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
    auto window = std::make_unique<X11Window>();
    if (!window->create(desc)) {
        return nullptr;
    }
    return window;
}

} // namespace tessera::hal
