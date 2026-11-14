// SPDX-License-Identifier: MIT
//
// Linux window backend selection (issue #155).
//
// A Linux build can carry the X11 backend, the Wayland backend, or both. The session the process is
// running in decides which one a window is created from: a Wayland compositor publishes
// `WAYLAND_DISPLAY` (and an X11 session publishes `DISPLAY`). The choice is made per window, not
// per process, so a test or tool can pin the backend for one creation.
//
// Selection order:
//   1. `TESSERA_WINDOW_BACKEND=x11|wayland` pins the backend for diagnostics and tests, with no
//      fallback: asking for a backend the build does not carry returns nullptr (the documented
//      "no window backend" contract). It is also how the X11 tests keep exercising X11 on a
//      Wayland desktop.
//   2. `WAYLAND_DISPLAY` set and the Wayland backend was compiled in -> Wayland. A failed Wayland
//      connection emits one `TESSERA_WINDOW_WAYLAND_UNSUPPORTED` diagnostic and the selector falls
//      through, so a Wayland session with a broken compositor still gets a window over XWayland.
//   3. Otherwise the X11 backend, when it was compiled in.
//   4. Otherwise nullptr.
//
// CI has `DISPLAY` (Xvfb) and no `WAYLAND_DISPLAY`, so it keeps exercising X11 unchanged.

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/window.hpp"

#if defined(TESSERA_HAS_X11)
#include "window_x11.hpp"
#endif
#if defined(TESSERA_HAS_WAYLAND)
#include "window_wayland.hpp"
#endif

#include <cmath>
#include <cstdlib>
#include <string_view>

namespace tessera::hal {
namespace {

#if defined(TESSERA_HAS_WAYLAND)
bool env_set(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}
#endif

enum class ForcedBackend { Auto, X11, Wayland };

ForcedBackend forced_backend() {
    const char* value = std::getenv("TESSERA_WINDOW_BACKEND");
    if (value == nullptr) {
        return ForcedBackend::Auto;
    }
    if (std::string_view(value) == "x11") {
        return ForcedBackend::X11;
    }
    if (std::string_view(value) == "wayland") {
        return ForcedBackend::Wayland;
    }
    return ForcedBackend::Auto;
}

} // namespace

// Default for platform windows without an OS event source: idle for the bounded timeout and report
// no event so a polling caller still throttles instead of spinning. Windows and the Linux backends
// override this with their own event wait.
bool Window::wait_for_events(double timeout_seconds) {
    if (std::isfinite(timeout_seconds) && timeout_seconds > 0.0) {
        wait_until_seconds(steady_now_seconds() + timeout_seconds);
    }
    return false;
}

std::unique_ptr<Window> create_window(const WindowDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        return nullptr;
    }
    switch (forced_backend()) {
    case ForcedBackend::Wayland:
#if defined(TESSERA_HAS_WAYLAND)
        return detail::create_wayland_window(desc);
#else
        return nullptr;
#endif
    case ForcedBackend::X11:
#if defined(TESSERA_HAS_X11)
        return detail::create_x11_window(desc);
#else
        return nullptr;
#endif
    case ForcedBackend::Auto:
        break;
    }
#if defined(TESSERA_HAS_WAYLAND)
    if (env_set("WAYLAND_DISPLAY")) {
        if (auto window = detail::create_wayland_window(desc)) {
            return window;
        }
        // The Wayland backend already emitted its single diagnostic; X11 remains the fallback.
    }
#endif
#if defined(TESSERA_HAS_X11)
    return detail::create_x11_window(desc);
#else
    return nullptr;
#endif
}

} // namespace tessera::hal
