// SPDX-License-Identifier: MIT
//
// Device-loss recovery on the real window path: a hidden window + swapchain presents one frame, one
// device-lost fault is injected through the Vulkan backend's existing fault seam so the next
// acquire reports SwapchainError::DeviceLost (the same value a real driver reset produces), and
// WindowHost must release the lost generation, let the runtime's controller recover, recreate the
// device and the presentation stack for the window's metrics, repaint a full frame and keep
// presenting. Skips when there is no window backend/presentation surface, the backend is not
// Vulkan, or the fault hooks are compiled out.

#include "tessera/app/recovery_policy.hpp"
#include "tessera/app/window.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#if defined(TESSERA_VULKAN_TEST_HOOKS)
#include "vulkan_internal.hpp"
#endif

#include <iostream>
#include <memory>
#include <string>
#include <utility>

#ifndef TESSERA_TEST_FONT
#error "TESSERA_TEST_FONT must point at the repository test font"
#endif

namespace {

using tessera::app::WindowDeviceState;
using tessera::app::WindowFrameStats;
using tessera::app::WindowHost;
using tessera::ui::Node;
using tessera::ui::PaintData;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_window_recovery: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    Node root(1);
    auto label_ptr = std::make_unique<Node>(2);
    Node& label = root.add_child(std::move(label_ptr));
    label.set_element_kind("Text");
    PaintData data;
    data.kind = tessera::ui::PaintKind::Text;
    data.text = "Recovered frame";
    label.set_paint_data(std::move(data));

    tessera::app::WindowOptions options;
    options.title = "Tessera device-loss recovery";
    options.width = 320;
    options.height = 200;
    options.visible = false;
    options.font_path = TESSERA_TEST_FONT;

    WindowHost host(root, compile_stylesheet("Text { font-size: 16px; }"), options);
    if (!host.available()) {
        std::cout << "app_window_recovery: skipped (no window backend or presentation surface)\n";
        return 0;
    }

#if !defined(TESSERA_VULKAN_TEST_HOOKS)
    std::cout << "app_window_recovery: skipped (TESSERA_VULKAN_TEST_HOOKS disabled)\n";
    return 0;
#else
    auto* vulkan_device =
        dynamic_cast<tessera::rendering::vulkan::VulkanDevice*>(&host.device_for_testing());
    if (vulkan_device == nullptr) {
        std::cout << "app_window_recovery: skipped (device is not the Vulkan backend)\n";
        return 0;
    }

    // One healthy frame first: the swapchain generation the fault destroys is a presented one.
    const WindowFrameStats before = host.run(1);
    if (!expect(before.frames >= 1, "the pre-loss frame is painted") ||
        !expect(before.presented >= 1, "the pre-loss frame is presented") ||
        !expect(before.recovered == 0 && before.device_state == WindowDeviceState::Ready,
                "a healthy run reports no recovery")) {
        return 1;
    }

    // Exactly one injected fault: the next acquire reports the loss and marks the device lost.
    vulkan_device->inject_fault_for_testing(
        tessera::rendering::vulkan::InjectedDeviceFault::DeviceLostOnAcquire);

    const WindowFrameStats after = host.run(2);
    bool ok = true;
    ok = expect(after.recovered >= 1, "the run recovered the lost device") && ok;
    ok = expect(after.recovery_attempts >= 1 &&
                    after.recovery_attempts <= tessera::app::kMaxDeviceRecoveryAttempts,
                "the recovery stayed inside the explicit attempt budget") &&
         ok;
    ok = expect(!after.stalled, "a successful recovery does not stall the run") && ok;
    ok = expect(after.device_state == WindowDeviceState::Ready,
                "the run reports a recovered device") &&
         ok;
    ok = expect(after.frames >= 2, "two frames were painted after the loss") && ok;
    ok = expect(after.presented >= 2, "two frames were presented after the loss") && ok;

    // The recreated device + swapchain keep working after run() returned.
    const WindowFrameStats third = host.run(1);
    ok = expect(third.presented >= 1 && third.frames >= 1,
                "the recreated presentation stack still presents") &&
         ok;
    ok = expect(third.device_state == WindowDeviceState::Ready,
                "a run after the recovery reports a healthy device") &&
         ok;

    if (!ok) {
        std::cerr << "app_window_recovery: failed\n";
        return 2;
    }
    std::cout << "app_window_recovery: ok (before=" << before.frames << " after=" << after.frames
              << " presented=" << after.presented << " recovered=" << after.recovered
              << " attempts=" << after.recovery_attempts << ")\n";
    return 0;
#endif
}
