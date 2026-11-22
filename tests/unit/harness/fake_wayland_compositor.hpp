// SPDX-License-Identifier: MIT
//
// A small in-process Wayland compositor (libwayland-server) for headless window tests: it
// advertises wl_output globals, answers the xdg-shell handshake and sends `wl_surface.enter` /
// `leave`, so the real client backend can be driven over a private socket with no panel.
//
// libwayland-server is not thread-safe, so the compositor runs on its own thread and the test
// thread talks to it through atomic command slots.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>
#include <wayland-server.h>

// The xdg-shell interfaces are compiled into the window backend from the same protocol XML; the
// generated code keeps them with C linkage and hidden visibility, which a static link permits.
extern "C" {
extern const struct wl_interface xdg_wm_base_interface;
extern const struct wl_interface xdg_surface_interface;
extern const struct wl_interface xdg_toplevel_interface;
}

namespace fake_wayland {

// --- xdg-shell request tables --------------------------------------------------------------------
// Request order is the protocol XML's; every entry has the arity libwayland passes.

struct SurfaceState {
    wl_resource* surface = nullptr;
    wl_resource* xdg_surface = nullptr;
    wl_resource* toplevel = nullptr;
    bool configured = false;
};

class FakeCompositor;
inline FakeCompositor* g_compositor = nullptr;

inline void noop2(wl_client*, wl_resource*) {}
inline void noop3_resource(wl_client*, wl_resource*, wl_resource*) {}
inline void noop3_u32(wl_client*, wl_resource*, std::uint32_t) {}
inline void noop3_str(wl_client*, wl_resource*, const char*) {}
inline void noop4_ii(wl_client*, wl_resource*, std::int32_t, std::int32_t) {}
inline void noop4_ru(wl_client*, wl_resource*, wl_resource*, std::uint32_t) {}
inline void noop5_ruu(wl_client*, wl_resource*, wl_resource*, std::uint32_t, std::uint32_t) {}
inline void noop6_ruii(wl_client*, wl_resource*, wl_resource*, std::uint32_t, std::int32_t,
                       std::int32_t) {}
inline void destroy_resource(wl_client*, wl_resource* resource) {
    wl_resource_destroy(resource);
}

// What one advertised wl_output reports. `subpixel` is the wire value of wl_output.subpixel.
struct OutputSpec {
    std::uint32_t subpixel = 0;
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t width = 1920;
    std::int32_t height = 1080;
    std::int32_t refresh_mhz = 60000;
    std::int32_t scale = 1;
    std::int32_t transform = 0;
};

class FakeCompositor {
public:
    // `entered` is the output the surface overlaps (-1: none, the window never gets an `enter`).
    FakeCompositor(const std::vector<OutputSpec>& outputs, int entered, const char* socket)
        : specs_(outputs),
          entered_(entered) {
        display_ = wl_display_create();
        loop_ = wl_display_get_event_loop(display_);
        socket_ok_ = wl_display_add_socket(display_, socket) == 0;
        output_resources_.assign(outputs.size(), nullptr);
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            // The global's user data is owned here, not by libwayland: `wl_global_create` stores
            // the pointer and never frees it, so the compositor keeps it in `output_data_` and
            // releases it in the destructor. Leaking it would be a real leak in the harness (LSan
            // reports it).
            output_data_.push_back(std::make_unique<OutputData>(OutputData{this, i}));
            globals_.push_back(wl_global_create(display_, &wl_output_interface, 2,
                                                output_data_.back().get(),
                                                &FakeCompositor::bind_output));
        }
        wl_global_create(display_, &wl_compositor_interface, 4, this,
                         &FakeCompositor::bind_compositor);
        wl_global_create(display_, &xdg_wm_base_interface, 1, this, &FakeCompositor::bind_wm_base);
        g_compositor = this;
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                wl_event_loop_dispatch(loop_, 5);
                apply_pending_move();
                apply_pending_reconfigure();
                apply_pending_removal();
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

    // `subpixels` is the wire value each advertised wl_output reports, one output per entry.
    FakeCompositor(const std::vector<std::uint32_t>& subpixels, int entered, const char* socket)
        : FakeCompositor(specs_from(subpixels), entered, socket) {}

    [[nodiscard]] bool ok() const {
        return socket_ok_;
    }

    // Asks the compositor thread to re-send an output's properties (a mode or scale change).
    void reconfigure(std::size_t output, const OutputSpec& spec) {
        reconfigure_spec_ = spec;
        pending_reconfigure_.store(static_cast<int>(output));
    }

    // Asks the compositor thread to remove an output global (hotplug).
    void remove_output(std::size_t output) {
        pending_removal_.store(static_cast<int>(output));
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
        send_output_state(resource, output->compositor->specs_[output->index], version);
    }

    static void send_output_state(wl_resource* resource, const OutputSpec& spec,
                                  std::uint32_t version) {
        wl_resource_post_event(resource, WL_OUTPUT_GEOMETRY, spec.x, spec.y, 340, 190,
                               static_cast<std::int32_t>(spec.subpixel), "Tessera", "Fake",
                               spec.transform);
        wl_resource_post_event(resource, WL_OUTPUT_MODE, 1u, spec.width, spec.height,
                               spec.refresh_mhz);
        if (version >= 2) {
            wl_resource_post_event(resource, WL_OUTPUT_SCALE, spec.scale);
            wl_resource_post_event(resource, WL_OUTPUT_DONE);
        }
    }

    static std::vector<OutputSpec> specs_from(const std::vector<std::uint32_t>& subpixels) {
        std::vector<OutputSpec> specs(subpixels.size());
        for (std::size_t i = 0; i < subpixels.size(); ++i) {
            specs[i].subpixel = subpixels[i];
        }
        return specs;
    }

    void apply_pending_reconfigure() {
        const int target = pending_reconfigure_.exchange(-1);
        if (target < 0 || static_cast<std::size_t>(target) >= output_resources_.size() ||
            output_resources_[static_cast<std::size_t>(target)] == nullptr) {
            return;
        }
        specs_[static_cast<std::size_t>(target)] = reconfigure_spec_;
        send_output_state(output_resources_[static_cast<std::size_t>(target)], reconfigure_spec_,
                          2);
    }

    void apply_pending_removal() {
        const int target = pending_removal_.exchange(-1);
        if (target >= 0 && static_cast<std::size_t>(target) < globals_.size()) {
            wl_global_remove(globals_[static_cast<std::size_t>(target)]);
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

    std::vector<OutputSpec> specs_;
    std::vector<wl_global*> globals_;
    // Owns the per-output user data handed to `wl_global_create`; see the constructor.
    std::vector<std::unique_ptr<OutputData>> output_data_;
    OutputSpec reconfigure_spec_;
    std::atomic<int> pending_reconfigure_{-1};
    std::atomic<int> pending_removal_{-1};
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

} // namespace fake_wayland
