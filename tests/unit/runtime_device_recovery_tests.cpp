// SPDX-License-Identifier: MIT
//
// D7 orchestration: RuntimeLoop drives the device-loss recovery state machine at frame boundaries
// with a fake rhi::Device (no GPU). Covers loss detection -> frames stop -> ResourcesOutstanding ->
// caller releases -> one bounded recovery -> full frame, plus the one-shot failure latch and the
// idle (no busy-wait) behaviour.

#include "tessera/runtime/device_recovery.hpp"
#include "tessera/runtime/runtime_loop.hpp"

#include <cstdint>
#include <expected>
#include <iostream>
#include <memory>

namespace {

using tessera::rendering::rhi::Backend;
using tessera::rendering::rhi::BufferDesc;
using tessera::rendering::rhi::CommandList;
using tessera::rendering::rhi::DeviceRecoveryError;
using tessera::rendering::rhi::DeviceStatus;
using tessera::rendering::rhi::PipelineDesc;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::Swapchain;
using tessera::rendering::rhi::SwapchainDesc;
using tessera::rendering::rhi::SwapchainError;
using tessera::rendering::rhi::TextureDesc;
using tessera::runtime::DeviceRecoveryController;
using tessera::runtime::DeviceRecoveryState;
using tessera::runtime::FixedStepConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::runtime::RuntimeLoopConfig;
using tessera::runtime::RuntimeTick;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "runtime_device_recovery: " << message << "\n";
    }
    return condition;
}

// Minimal rhi::Device: no resources, programmer-controlled status()/recover(). Resource creation
// returns null because the test only exercises the loss/recovery seam.
class FakeDevice final : public tessera::rendering::rhi::Device {
public:
    [[nodiscard]] Backend backend() const override {
        return Backend::Null;
    }
    std::unique_ptr<tessera::rendering::rhi::Buffer> create_buffer(const BufferDesc&) override {
        return nullptr;
    }
    std::unique_ptr<tessera::rendering::rhi::Texture> create_texture(const TextureDesc&) override {
        return nullptr;
    }
    std::unique_ptr<tessera::rendering::rhi::Pipeline>
    create_pipeline(const PipelineDesc&) override {
        return nullptr;
    }
    std::unique_ptr<RenderTarget> create_render_target(const RenderTargetDesc&) override {
        return nullptr;
    }
    std::unique_ptr<CommandList> create_command_list() override {
        return nullptr;
    }
    std::expected<std::unique_ptr<Swapchain>, SwapchainError>
    create_swapchain(const SwapchainDesc&) override {
        return std::unexpected(SwapchainError::NotSupported);
    }
    void wait_idle() override {}

    [[nodiscard]] DeviceStatus status() const override {
        return status_;
    }
    std::expected<void, DeviceRecoveryError> recover() override {
        ++recover_calls;
        if (!resources_released) {
            return std::unexpected(DeviceRecoveryError::ResourcesOutstanding);
        }
        if (fail_recoveries > 0) {
            --fail_recoveries;
            return std::unexpected(DeviceRecoveryError::DeviceCreationFailed);
        }
        status_ = DeviceStatus::Ready;
        ++generation;
        return {};
    }

    void lose() {
        status_ = DeviceStatus::Lost;
    }
    // Simulates an out-of-band recovery (e.g. an application-level device reset) so the controller
    // can observe Ready and allow a later loss to make a fresh attempt.
    void external_recovery() {
        status_ = DeviceStatus::Ready;
        ++generation;
    }

    DeviceStatus status_ = DeviceStatus::Ready;
    bool resources_released = false;
    int fail_recoveries = 0;
    int recover_calls = 0;
    std::uint32_t generation = 0;
};

RuntimeLoopConfig loop_config(DeviceRecoveryController& controller) {
    RuntimeLoopConfig config;
    config.fixed_step = FixedStepConfig{60.0, 250.0, 5, 1.0};
    config.render_on_demand = true;
    config.device_recovery = &controller;
    return config;
}

} // namespace

int main() {
    FakeDevice device;
    DeviceRecoveryController controller(device);
    ManualClock clock;
    RuntimeLoop loop(clock, loop_config(controller));

    bool ok = expect(controller.state() == DeviceRecoveryState::Ready, "controller starts Ready");

    // Healthy loop: no recovery activity, normal render scheduling.
    RuntimeTick tick = loop.tick();
    ok = expect(!tick.device_blocked && tick.device_state == DeviceRecoveryState::Ready,
                "healthy tick is not blocked") &&
         ok;
    ok = expect(tick.idle && !tick.should_render, "healthy on-demand tick idles") && ok;
    ok = expect(device.recover_calls == 0, "healthy device is not probed") && ok;
    loop.mark_render_dirty();
    tick = loop.tick();
    ok = expect(tick.should_render && !tick.idle, "dirty frame renders") && ok;
    loop.clear_render_dirty();

    // Loss: frames stop immediately, one cheap recovery probe per tick while the caller still owns
    // lost-generation resources. Each blocked tick reports idle so the caller waits (bounded)
    // instead of spinning.
    device.lose();
    loop.request_frame();
    tick = loop.tick();
    ok = expect(tick.device_blocked && !tick.should_render, "lost tick stops rendering") && ok;
    ok = expect(tick.device_state == DeviceRecoveryState::WaitingForCaller,
                "outstanding resources -> WaitingForCaller") &&
         ok;
    ok = expect(tick.idle, "blocked tick is idle (no busy-wait)") && ok;
    ok = expect(device.recover_calls == 1 && controller.attempts() == 1,
                "exactly one recovery probe in the loss tick") &&
         ok;
    tick = loop.tick();
    ok = expect(tick.device_blocked && tick.device_state == DeviceRecoveryState::WaitingForCaller,
                "still waiting for the caller") &&
         ok;
    ok = expect(device.recover_calls == 2, "still at most one probe per tick") && ok;

    // Caller releases its RHI objects: the next tick recovers once and gives the caller the tick
    // to recreate the swapchain/resources from the generation bump.
    device.resources_released = true;
    tick = loop.tick();
    ok = expect(tick.device_recovered && tick.device_generation == 1,
                "recovery succeeds and bumps the generation") &&
         ok;
    ok = expect(!tick.should_render && tick.device_state == DeviceRecoveryState::Ready,
                "recovery tick does not render") &&
         ok;
    ok = expect(tick.full_frame && !tick.idle, "recovery tick flags a full repaint") && ok;
    ok = expect(device.recover_calls == 3, "one attempt on the recovering tick") && ok;

    tick = loop.tick();
    ok = expect(tick.should_render && tick.full_frame, "next tick paints a full frame") && ok;
    ok = expect(tick.device_generation == 1, "generation is stable after recovery") && ok;
    tick = loop.tick();
    ok = expect(!tick.should_render && tick.idle && !tick.full_frame, "full frame is one-shot") &&
         ok;

    // Failure path: the one-shot recovery fails, frames stay stopped, and no further attempts are
    // made until the device is observed Ready again (a new loss).
    device.lose();
    device.fail_recoveries = 1;
    tick = loop.tick();
    ok = expect(tick.device_failed && tick.device_state == DeviceRecoveryState::Failed,
                "failed recovery -> Failed") &&
         ok;
    ok = expect(!tick.should_render && tick.idle, "failed device keeps frames stopped") && ok;
    const int calls_after_failure = device.recover_calls;
    tick = loop.tick();
    ok = expect(tick.device_state == DeviceRecoveryState::Failed && tick.device_failed,
                "Failed persists without attempts") &&
         ok;
    ok = expect(device.recover_calls == calls_after_failure, "no further attempts while Failed") &&
         ok;

    // External recovery is observed; a subsequent loss gets a fresh one-shot attempt.
    device.external_recovery();
    tick = loop.tick();
    ok = expect(tick.device_state == DeviceRecoveryState::Ready && !tick.device_blocked,
                "observed Ready clears Failed") &&
         ok;
    device.lose();
    tick = loop.tick();
    ok = expect(tick.device_recovered && tick.device_generation == 2,
                "new loss attempts recovery again") &&
         ok;

    // Continuous mode must also stop rendering while the device is lost, and idle instead.
    loop.set_continuous(true);
    device.resources_released = false;
    device.lose();
    tick = loop.tick();
    ok = expect(tick.device_blocked && !tick.should_render && tick.idle,
                "continuous mode stops presenting while lost") &&
         ok;
    device.resources_released = true;
    tick = loop.tick();
    ok = expect(tick.device_recovered, "continuous mode recovers") && ok;
    tick = loop.tick();
    ok = expect(tick.should_render && tick.full_frame, "continuous mode resumes after recovery") &&
         ok;

    // Detach: the loop goes back to plain behaviour.
    loop.set_device_recovery(nullptr);
    device.lose();
    tick = loop.tick();
    ok = expect(!tick.device_blocked && tick.should_render, "detached loop ignores device loss") &&
         ok;

    if (!ok) {
        std::cerr << "runtime_device_recovery: failed\n";
        return 1;
    }
    std::cout << "runtime_device_recovery: ok\n";
    return 0;
}
