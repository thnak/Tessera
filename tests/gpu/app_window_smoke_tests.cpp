// SPDX-License-Identifier: MIT
//
// App-window smoke: the WindowHost creates a hidden platform window plus a Vulkan swapchain, runs
// the AppHost loop and presents N forced frames. Skips when the platform has no window backend or
// no presentation surface (the host reports available() == false instead of failing). A second
// window requests 4x MSAA: the AA policy and the device capability decide, and the window must
// present either way (S8).

#include "tessera/app/window.hpp"
#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_smoke: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    // --- The default presentation: one sample, byte-identical to the pre-S8 frame path ----------
    {
        Node root(1);
        auto label_ptr = std::make_unique<Node>(2);
        Node& label = root.add_child(std::move(label_ptr));
        label.set_element_kind("Text");
        PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = "App window smoke";
        label.set_paint_data(std::move(data));

        tessera::app::WindowOptions options;
        options.title = "Tessera app-window smoke";
        options.width = 320;
        options.height = 200;
        options.visible = false;
        options.font_path = TESSERA_TEST_FONT;

        tessera::app::WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"),
                                      options);
        if (!host.available()) {
            std::cout << "app_window_smoke: skipped (no window backend or presentation surface)\n";
            return 0;
        }

        const tessera::app::WindowFrameStats stats = host.run(3);
        if (!expect(stats.frames >= 3, "the loop painted at least three frames") ||
            !expect(stats.presented >= 3, "the loop presented at least three frames")) {
            return 1;
        }

        // run() is re-entrant: the input queue is reattached on entry and teardown drains the
        // submitted lists without a device-wide wait, so a second bounded run still produces
        // frames.
        const tessera::app::WindowFrameStats second = host.run(2);
        if (!expect(second.frames >= 2, "a second run paints frames") ||
            !expect(second.presented >= 2, "a second run presents frames")) {
            return 2;
        }

        // The window resolves and applies the display-aware AA policy between frames
        // (display-aware-rendering.md section 6): the painted frame reports the device scale and a
        // policy whose scale is that device scale quantized. The default switch is on, so text uses
        // the policy's hinting rather than the golden font hints.
        using tessera::ui::scale::quantize_render_scale;
        using tessera::ui::scale::TextHinting;
        if (!expect(stats.device_scale == host.host().dpi_scale(),
                    "the reported device scale is the window's metrics scale") ||
            !expect(stats.aa_policy.render_scale == quantize_render_scale(stats.device_scale),
                    "the policy's render scale is the quantized device scale") ||
            !expect(stats.aa_policy.hinting != TextHinting::Font,
                    "the default window applies the policy, not the golden font hints") ||
            !expect(stats.content_scale_x == 1.0f && stats.content_scale_y == 1.0f,
                    "a 1.0 window reports the identity frame transform") ||
            !expect(stats.sample_count == 1,
                    "the default window presents a single sample (the byte-identical path)")) {
            return 3;
        }
        std::cout << "app_window_smoke: ok (frames=" << stats.frames
                  << " presented=" << stats.presented << " idle_waits=" << stats.idle_waits
                  << " second_frames=" << second.frames << " device_scale=" << stats.device_scale
                  << " aa_scale=" << stats.aa_policy.render_scale
                  << " content_scale=" << stats.content_scale_x << " samples=" << stats.sample_count
                  << ")\n";
    }

    // --- A 4x request: the AA policy and the device capability decide (S8) ----------------------
    // At a render scale below kMsaaMaxScale with AA on, the window presents 4x when the device
    // supports it (the scene renders into a 4x offscreen target and is presented through its
    // resolved image); otherwise it keeps the single-sample path. Either way the window must
    // present frames, so this proves the wiring without turning an unsupported device into a
    // failure.
    {
        Node root(1);
        auto label_ptr = std::make_unique<Node>(2);
        Node& label = root.add_child(std::move(label_ptr));
        label.set_element_kind("Text");
        PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = "MSAA presentation";
        label.set_paint_data(std::move(data));

        tessera::app::WindowOptions options;
        options.title = "Tessera app-window MSAA smoke";
        options.width = 320;
        options.height = 200;
        options.visible = false;
        options.font_path = TESSERA_TEST_FONT;
        options.msaa_samples = 4;

        tessera::app::WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"),
                                      options);
        if (!host.available()) {
            std::cout << "app_window_smoke: msaa skipped (no presentation surface)\n";
            return 0;
        }
        const tessera::app::WindowFrameStats stats = host.run(3);
        if (!expect(stats.frames >= 3, "the msaa request still paints frames") ||
            !expect(stats.presented >= 3, "the msaa request still presents frames") ||
            !expect(stats.sample_count == 1 || stats.aa_policy.msaa_samples == 4,
                    "a 4x presentation is only reported when the policy resolved 4x") ||
            !expect(stats.sample_count == 4 || stats.sample_count == 1,
                    "the presentation sample count is 1 or 4")) {
            return 4;
        }
        std::cout << "app_window_smoke: msaa sample_count=" << stats.sample_count
                  << " frames=" << stats.frames << " presented=" << stats.presented << "\n";
    }
    return 0;
}
