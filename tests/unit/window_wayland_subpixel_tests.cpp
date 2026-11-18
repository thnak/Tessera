// SPDX-License-Identifier: MIT
//
// Wayland subpixel geometry (issue #504, display-aware-rendering.md section 2): the
// `wl_output.geometry` subpixel value of the output a window is on reaches
// `WindowMetrics::pixel_geometry`.
//
// Headless and panel-free: weston's headless backend always reports `unknown`, so this test hosts a
// small fake compositor (libwayland-server) in-process that advertises wl_output globals with the
// subpixel value under test, answers the xdg-shell handshake, and sends `wl_surface.enter` /
// `leave` for the output the window is on. The real client backend connects to it over a private
// socket. Whether a real driver reports the right stripe for a real panel is a manual check.
//
// libwayland-server is not thread-safe, so the compositor runs on its own thread and the test
// thread talks to it through one atomic command slot.

#include "tessera/hal/display.hpp"
#include "tessera/hal/window.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <wayland-server.h>

// The xdg-shell interfaces are compiled into the window backend from the same protocol XML; the
// generated code keeps them with C linkage and hidden visibility, which a static link permits.
extern "C" {
extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;
}

namespace {

using tessera::hal::PixelGeometry;
using tessera::hal::WindowEvent;
using tessera::hal::WindowEventType;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "window_wayland_subpixel: " << message << "\n";
        ++failures;
    }
}

// --- xdg-shell request tables --------------------------------------------------------------------
// Request order is the protocol XML's; every entry has the arity libwayland passes.

struct SurfaceState {
    wl_resource* surface = nullptr;
    wl_resource* xdg_surface = nullptr;
    wl_resource* toplevel = nullptr;
    bool configured = false;
};

class FakeCompositor;
FakeCompositor* g_compositor = nullptr;

void noop2(wl_client*, wl_resource*) {}
void noop3_resource(wl_client*, wl_resource*, wl_resource*) {}
void noop3_u32(wl_client*, wl_resource*, std::uint32_t) {}
void noop3_str(wl_client*, wl_resource*, const char*) {}
void noop4_ii(wl_client*, wl_resource*, std::int32_t, std::int32_t) {}
void noop4_ru(wl_client*, wl_resource*, wl_resource*, std::uint32_t) {}
void noop5_ruu(wl_client*, wl_resource*, wl_resource*, std::uint32_t, std::uint32_t) {}
void noop6_ruii(wl_client*, wl_resource*, wl_resource*, std::uint32_t, std::int32_t, std::int32_t) {
}
void destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

class FakeCompositor {
public:
    // `subpixels` is the wire value each advertised wl_output reports, one output per entry.
    // `entered` is the output the surface overlaps (-1: none, the window never gets an `enter`).
    FakeCompositor(const std::vector<std::uint32_t>& subpixels, int entered, const char* socket)
        : subpixels_(subpixels),
          entered_(entered) {
        display_ = wl_display_create();
        loop_ = wl_display_get_event_loop(display_);
        socket_ok_ = wl_display_add_socket(display_, socket) == 0;
        output_resources_.assign(subpixels.size(), nullptr);
        for (std::size_t i = 0; i < subpixels.size(); ++i) {
            wl_global_create(display_, &wl_output_interface, 2, new OutputData{this, i},
                             &FakeCompositor::bind_output);
        }
        wl_global_create(display_, &wl_compositor_interface, 4, this,
                         &FakeCompositor::bind_compositor);
        wl_global_create(display_, &xdg_wm_base_interface, 1, this, &FakeCompositor::bind_wm_base);
        g_compositor = this;
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                wl_event_loop_dispatch(loop_, 5);
                apply_pending_move();
                wl_display_flush_clients(display_);
            }
        });
    }

    ~FakeCompositor() {
        stop_.store(true);
        thread_.join();
        wl_display_destroy_clients(display_);
        wl_display_destroy(display_);
        g_compositor = nullptr;
    }

    [[nodiscard]] bool ok() const {
        return socket_ok_;
    }

    // Asks the compositor thread to move the surface to another output.
    void move_to(int output) {
        pending_move_.store(output);
    }

private:
    struct OutputData {
        FakeCompositor* compositor;
        std::size_t index;
    };

    static void bind_output(wl_client* client, void* data, std::uint32_t version,
                            std::uint32_t id) {
        auto* output = static_cast<OutputData*>(data);
        wl_resource* resource =
            wl_resource_create(client, &wl_output_interface, static_cast<int>(version), id);
        output->compositor->output_resources_[output->index] = resource;
        wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);
        wl_resource_post_event(
            resource, WL_OUTPUT_GEOMETRY, 0, 0, 340, 190,
            static_cast<std::int32_t>(output->compositor->subpixels_[output->index]), "Tessera",
            "Fake", 0);
        if (version >= 2) {
            wl_resource_post_event(resource, WL_OUTPUT_DONE);
        }
    }

    static void bind_compositor(wl_client* client, void* data, std::uint32_t version,
                                std::uint32_t id) {
        wl_resource* resource =
            wl_resource_create(client, &wl_compositor_interface, static_cast<int>(version), id);
        static const struct wl_compositor_interface kImpl = {&FakeCompositor::create_surface,
                                                             noop3_u32};
        wl_resource_set_implementation(resource, &kImpl, data, nullptr);
    }

    static void create_surface(wl_client* client, wl_resource* compositor, std::uint32_t id) {
        auto* state = new SurfaceState;
        state->surface = wl_resource_create(client, &wl_surface_interface,
                                            wl_resource_get_version(compositor), id);
        static const struct wl_surface_interface kImpl = {
            destroy_resource, noop5_ruu_attach,        noop_damage, noop3_u32, noop3_resource,
            noop3_resource,   &FakeCompositor::commit, noop3_i32,   noop3_i32, noop_damage,
            noop4_ii};
        wl_resource_set_implementation(state->surface, &kImpl, state, [](wl_resource* resource) {
            delete static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
        });
    }

    static void noop5_ruu_attach(wl_client*, wl_resource*, wl_resource*, std::int32_t,
                                 std::int32_t) {}
    static void noop_damage(wl_client*, wl_resource*, std::int32_t, std::int32_t, std::int32_t,
                            std::int32_t) {}
    static void noop3_i32(wl_client*, wl_resource*, std::int32_t) {}

    static void commit(wl_client*, wl_resource* resource) {
        auto* state = static_cast<SurfaceState*>(wl_resource_get_user_data(resource));
        if (state->toplevel == nullptr || state->configured || g_compositor == nullptr) {
            return;
        }
        state->configured = true;
        FakeCompositor& self = *g_compositor;
        self.surface_ = state->surface;
        if (self.entered_ >= 0) {
            self.current_ = self.entered_;
            wl_resource_post_event(state->surface, WL_SURFACE_ENTER,
                                   self.output_resources_[static_cast<std::size_t>(self.entered_)]);
        }
        wl_array states;
        wl_array_init(&states);
        wl_resource_post_event(state->toplevel, 0, 320, 200, &states);
        wl_array_release(&states);
        wl_resource_post_event(state->xdg_surface, 0, wl_display_next_serial(self.display_));
    }

    static void bind_wm_base(wl_client* client, void* data, std::uint32_t version,
                             std::uint32_t id) {
        wl_resource* resource =
            wl_resource_create(client, &xdg_wm_base_interface, static_cast<int>(version), id);
        struct WmBaseImpl {
            void (*destroy)(wl_client*, wl_resource*);
            void (*create_positioner)(wl_client*, wl_resource*, std::uint32_t);
            void (*get_xdg_surface)(wl_client*, wl_resource*, std::uint32_t, wl_resource*);
            void (*pong)(wl_client*, wl_resource*, std::uint32_t);
        };
        static const WmBaseImpl kImpl = {destroy_resource, noop3_u32,
                                         &FakeCompositor::get_xdg_surface, noop3_u32};
        wl_resource_set_implementation(resource, &kImpl, data, nullptr);
    }

    static void get_xdg_surface(wl_client* client, wl_resource* wm_base, std::uint32_t id,
                                wl_resource* surface) {
        auto* state = static_cast<SurfaceState*>(wl_resource_get_user_data(surface));
        state->xdg_surface = wl_resource_create(client, &xdg_surface_interface,
                                                wl_resource_get_version(wm_base), id);
        struct XdgSurfaceImpl {
            void (*destroy)(wl_client*, wl_resource*);
            void (*get_toplevel)(wl_client*, wl_resource*, std::uint32_t);
            void (*get_popup)(wl_client*, wl_resource*, std::uint32_t, wl_resource*, wl_resource*);
            void (*set_window_geometry)(wl_client*, wl_resource*, std::int32_t, std::int32_t,
                                        std::int32_t, std::int32_t);
            void (*ack_configure)(wl_client*, wl_resource*, std::uint32_t);
        };
        static const XdgSurfaceImpl kImpl = {
            destroy_resource, &FakeCompositor::get_toplevel,
            [](wl_client*, wl_resource*, std::uint32_t, wl_resource*, wl_resource*) {}, noop_damage,
            noop3_u32};
        wl_resource_set_implementation(state->xdg_surface, &kImpl, state, nullptr);
    }

    static void get_toplevel(wl_client* client, wl_resource* xdg_surface, std::uint32_t id) {
        auto* state = static_cast<SurfaceState*>(wl_resource_get_user_data(xdg_surface));
        state->toplevel = wl_resource_create(client, &xdg_toplevel_interface,
                                             wl_resource_get_version(xdg_surface), id);
        struct ToplevelImpl {
            void (*destroy)(wl_client*, wl_resource*);
            void (*set_parent)(wl_client*, wl_resource*, wl_resource*);
            void (*set_title)(wl_client*, wl_resource*, const char*);
            void (*set_app_id)(wl_client*, wl_resource*, const char*);
            void (*show_window_menu)(wl_client*, wl_resource*, wl_resource*, std::uint32_t,
                                     std::int32_t, std::int32_t);
            void (*move)(wl_client*, wl_resource*, wl_resource*, std::uint32_t);
            void (*resize)(wl_client*, wl_resource*, wl_resource*, std::uint32_t, std::uint32_t);
            void (*set_max_size)(wl_client*, wl_resource*, std::int32_t, std::int32_t);
            void (*set_min_size)(wl_client*, wl_resource*, std::int32_t, std::int32_t);
            void (*set_maximized)(wl_client*, wl_resource*);
            void (*unset_maximized)(wl_client*, wl_resource*);
            void (*set_fullscreen)(wl_client*, wl_resource*, wl_resource*);
            void (*unset_fullscreen)(wl_client*, wl_resource*);
            void (*set_minimized)(wl_client*, wl_resource*);
        };
        static const ToplevelImpl kImpl = {destroy_resource,
                                           noop3_resource,
                                           noop3_str,
                                           noop3_str,
                                           noop6_ruii,
                                           noop4_ru,
                                           noop5_ruu,
                                           noop4_ii,
                                           noop4_ii,
                                           noop2,
                                           noop2,
                                           noop3_resource,
                                           noop2,
                                           noop2};
        wl_resource_set_implementation(state->toplevel, &kImpl, state, nullptr);
    }

    void apply_pending_move() {
        const int target = pending_move_.exchange(-1);
        if (target < 0 || surface_ == nullptr || target == current_) {
            return;
        }
        if (current_ >= 0) {
            wl_resource_post_event(surface_, WL_SURFACE_LEAVE,
                                   output_resources_[static_cast<std::size_t>(current_)]);
        }
        wl_resource_post_event(surface_, WL_SURFACE_ENTER,
                               output_resources_[static_cast<std::size_t>(target)]);
        current_ = target;
    }

    std::vector<std::uint32_t> subpixels_;
    std::vector<wl_resource*> output_resources_;
    wl_display* display_ = nullptr;
    wl_event_loop* loop_ = nullptr;
    wl_resource* surface_ = nullptr;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<int> pending_move_{-1};
    int entered_ = -1;
    int current_ = -1;
    bool socket_ok_ = false;
};

std::unique_ptr<tessera::hal::Window> open_window(const char* label) {
    tessera::hal::WindowDesc desc;
    desc.title = std::string("Tessera Wayland subpixel ") + label;
    desc.width = 320;
    desc.height = 200;
    desc.visible = false;
    return tessera::hal::create_window(desc);
}

PixelGeometry single_output(std::uint32_t wire, const char* label, int index) {
    const std::string socket =
        "tessera-subpx-" + std::to_string(getpid()) + "-" + std::to_string(index);
    FakeCompositor compositor({wire}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, std::string(label) + ": fake compositor socket");
        return PixelGeometry::Unknown;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window(label);
    if (window == nullptr) {
        expect(false, std::string(label) + ": create_window");
        return PixelGeometry::Unknown;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    return window->metrics().pixel_geometry;
}

void check_two_outputs() {
    const std::string socket = "tessera-subpx-" + std::to_string(getpid()) + "-pair";
    // Output 0 is horizontal RGB, output 1 is horizontal BGR; the surface enters output 1.
    FakeCompositor compositor({2, 3}, 1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "pair: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("pair");
    if (window == nullptr) {
        expect(false, "pair: create_window");
        return;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    expect(window->metrics().pixel_geometry == PixelGeometry::BgrHorizontal,
           "two outputs: the window takes the one it entered, not the first");

    // Moving to the other output retracts the old answer and raises a DisplayChanged.
    compositor.move_to(0);
    bool saw_display_changed = false;
    for (int attempt = 0; attempt < 200 && !saw_display_changed; ++attempt) {
        (void)window->wait_for_events(0.01);
        const std::size_t count = window->poll_events(events);
        for (std::size_t i = 0; i < count; ++i) {
            saw_display_changed |= events[i].type == WindowEventType::DisplayChanged;
        }
    }
    expect(saw_display_changed, "moving to another output raises DisplayChanged");
    expect(window->metrics().pixel_geometry == PixelGeometry::RgbHorizontal,
           "after the move the window takes the new output's layout");
}

void check_ambiguous() {
    const std::string socket = "tessera-subpx-" + std::to_string(getpid()) + "-amb";
    // Two outputs and no `enter` yet: neither is evidence, so the answer is Unknown.
    FakeCompositor compositor({2, 3}, -1, socket.c_str());
    if (!compositor.ok()) {
        expect(false, "ambiguous: fake compositor socket");
        return;
    }
    setenv("WAYLAND_DISPLAY", socket.c_str(), 1);
    auto window = open_window("ambiguous");
    if (window == nullptr) {
        expect(false, "ambiguous: create_window");
        return;
    }
    std::array<WindowEvent, 32> events{};
    (void)window->poll_events(events);
    expect(window->metrics().pixel_geometry == PixelGeometry::Unknown,
           "two outputs and no enter is Unknown, never the first output's stripe");
}

} // namespace

int main() {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    char directory[] = "/tmp/tessera-wl-XXXXXX";
    if (runtime == nullptr || runtime[0] == '\0' || access(runtime, W_OK) != 0) {
        if (mkdtemp(directory) == nullptr) {
            std::cout << "window_wayland_subpixel: skipped (no writable XDG_RUNTIME_DIR)\n";
            return 0;
        }
        setenv("XDG_RUNTIME_DIR", directory, 1);
    }
    setenv("TESSERA_WINDOW_BACKEND", "wayland", 1);

    // wl_output.subpixel: unknown 0, none 1, horizontal_rgb 2, horizontal_bgr 3, vertical_rgb 4,
    // vertical_bgr 5; 77 is a value a newer compositor might send.
    expect(single_output(2, "hrgb", 0) == PixelGeometry::RgbHorizontal, "horizontal_rgb");
    expect(single_output(3, "hbgr", 1) == PixelGeometry::BgrHorizontal, "horizontal_bgr");
    expect(single_output(1, "none", 2) == PixelGeometry::None, "none");
    expect(single_output(4, "vrgb", 3) == PixelGeometry::RgbVertical, "vertical_rgb");
    expect(single_output(0, "unknown", 4) == PixelGeometry::Unknown, "unknown");
    expect(single_output(77, "future", 5) == PixelGeometry::Unknown,
           "an unrecognised wire value is Unknown, not a guessed stripe");
    check_two_outputs();
    check_ambiguous();

    if (failures == 0) {
        std::cout << "window_wayland_subpixel: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
