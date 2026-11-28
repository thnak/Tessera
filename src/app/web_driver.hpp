// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/app/host.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/scene/node_fwd.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "web_host.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

// The web app driver (issue #1023, ADR-046): the browser counterpart of the Vulkan `WindowHost`. It
// runs the same `AppHost` frame (style, layout, animation, damage, paint) for one scene root, with
// the page's WebGPU device and canvas surface instead of a swapchain, `WebHost`'s normalized input
// queue instead of the window's, and the text, vector and glass stacks on that device. Emscripten
// only; the host-testable pieces it composes (`WebHost`, `backing_extent`) have their own tests.
//
// Without a WebGPU device (Node, a browser without WebGPU) it still runs the whole non-GPU frame:
// fonts are measured, the stylesheet resolves, layout runs and input is dispatched, so a Node check
// exercises the app logic and only the canvas is missing. `gpu()` says which case it is.
//
// Nothing blocks the UI thread (ADR-021): a command list is single-use, so each frame records a
// fresh one, submits it with `submit_async` and keeps at most two in flight, polled with
// `is_complete`. A frame that finds both in flight stays owed and asks for the next callback.

namespace tessera::app {

struct WebAppOptions {
    // CSS selector of the canvas the page created (tessera_web_host.js uses "#tessera-canvas").
    std::string canvas_selector = "#tessera-canvas";
    // Font files in the Emscripten file system (preloaded next to the page): the first is the
    // primary face, each further one another weight of the family.
    std::vector<std::string> font_paths;
    // Borrowed; must outlive the driver.
    const ui::assets::AssetBundle* vector_assets = nullptr;
    ui::UpdateScheduler* scheduler = nullptr;
    // Called once, after the host exists and before the first frame (themes, metrics).
    std::function<void(AppHost&)> configure_host;
    // Prefix of the one stderr line per submitted canvas frame the browser check counts.
    std::string log_prefix = "web_app";
};

struct WebAppStats {
    unsigned frames = 0;   // frame callbacks run
    unsigned events = 0;   // input events consumed
    unsigned presents = 0; // canvas frames submitted
};

class WebAppDriver {
public:
    // Null only when a font cannot be loaded. A missing or failing GPU degrades to the headless
    // mode described above instead of failing.
    [[nodiscard]] static std::unique_ptr<WebAppDriver> create(ui::Node& root,
                                                              ui::style::CompiledStyleSheet sheet,
                                                              WebAppOptions options,
                                                              hal::web::WebHost& web);
    ~WebAppDriver();
    WebAppDriver(const WebAppDriver&) = delete;
    WebAppDriver& operator=(const WebAppDriver&) = delete;

    // The `WebHost` frame step: input, loop tick, the host frame and, when something changed, one
    // canvas frame.
    hal::web::WebFrameResult run_frame(double now_seconds);

    [[nodiscard]] bool gpu() const;
    [[nodiscard]] const WebAppStats& stats() const;
    [[nodiscard]] AppHost& host();

private:
    struct Impl;
    explicit WebAppDriver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::app
