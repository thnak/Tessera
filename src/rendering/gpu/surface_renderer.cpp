// SPDX-License-Identifier: MIT
//
// `rendering::SurfaceRenderer` (ADR-050 section 7, roadmap AF6, issue #178): the renderer half of
// "a component gets a typed ref to a native surface and its developer class draws into it".
//
// The shape follows `webgpu_canvas_target.cpp`, which wraps a *host-owned* texture view as an RHI
// render target and draws into it rather than allocating its own. Here the host-owned half is a
// `hal::NativeSurface` (a borrowed handle pair plus a frozen `SurfaceDesc`) and the destination is
// a caller-supplied RHI target, so nothing in this file knows whether the pixels end up in a
// swapchain image, a test's offscreen target or a canvas.
//
// Two properties are load-bearing and both are asserted by the tests:
//
//  * the surface is never owned. `hal::NativeSurface` is a weak view, so a renderer held across a
//    window's destruction reports `NoSurface` instead of touching a released handle.
//  * nothing blocks. A slot still in flight is skipped (`SlotBusy`); there is no `wait_idle` and no
//    fence wait anywhere in this translation unit.

#include "tessera/rendering/gpu/surface_renderer.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::rendering {

SurfaceRenderer::SurfaceRenderer(rhi::Device& device, hal::NativeSurface surface,
                                 SurfaceRendererConfig config)
    : device_(&device),
      surface_(std::move(surface)),
      config_(config) {
    // A slot count of zero would skip every frame, which is a configuration bug rather than a
    // runtime state: fail at construction instead of in the loop.
    if (config_.frames_in_flight == 0) {
        throw std::invalid_argument("SurfaceRenderer: frames_in_flight must be at least 1");
    }
    slots_.resize(config_.frames_in_flight);
}

SurfaceRenderer::~SurfaceRenderer() = default;

bool SurfaceRenderer::valid() const {
    return device_ != nullptr && surface_.valid() && device_->status() == rhi::DeviceStatus::Ready;
}

hal::SurfaceDesc SurfaceRenderer::surface_desc() const {
    return surface_.desc();
}

bool SurfaceRenderer::native_presentable() const {
    const hal::SurfaceDesc desc = surface_.desc();
    return desc.valid() && desc.window_system != hal::WindowSystem::NoWindow;
}

rhi::Extent2D SurfaceRenderer::extent() const {
    const hal::SurfaceDesc desc = surface_.desc();
    return desc.valid() ? rhi::Extent2D{desc.width_px, desc.height_px} : rhi::Extent2D{};
}

rhi::RenderTargetDesc SurfaceRenderer::target_desc() const {
    rhi::RenderTargetDesc desc;
    desc.extent = extent();
    desc.format = config_.format;
    desc.clear_color = config_.clear_color;
    desc.load_op = rhi::LoadOp::Clear;
    desc.store_op = rhi::StoreOp::Store;
    return desc;
}

void SurfaceRenderer::rebind(hal::NativeSurface surface) {
    surface_ = std::move(surface);
    // The old target belongs to the previous extent generation. Dropping it is safe even with a
    // submission in flight: the RHI keeps a command list's resources alive until that list's own
    // submission completes, and the replacement target is only created on the next frame.
    drop_target();
}

void SurfaceRenderer::drop_target() noexcept {
    target_.reset();
    in_frame_ = false;
    frame_commands_ = nullptr;
    frame_destination_ = nullptr;
    // The slot lists are kept: they may still be in flight, and their own completion is the only
    // thing that may recycle them.
}

bool SurfaceRenderer::prepare_target() {
    const rhi::Extent2D wanted = extent();
    if (wanted.width == 0 || wanted.height == 0) {
        return false;
    }
    if (target_ != nullptr && target_->extent().width == wanted.width &&
        target_->extent().height == wanted.height && target_->format() == config_.format) {
        return true;
    }
    // Recreate rather than reuse on a resize: the pass clears anyway, so a fresh target costs one
    // allocation per resize and never composites stale geometry.
    target_ = device_->create_render_target(target_desc());
    return target_ != nullptr;
}

rhi::CommandList* SurfaceRenderer::free_slot() {
    for (auto& slot : slots_) {
        // A slot whose own list reports complete is reusable. `is_complete()` is a poll, so this
        // loop never blocks the UI thread (ADR-021).
        if (slot == nullptr || slot->is_complete()) {
            slot = device_->create_command_list();
            return slot.get();
        }
    }
    return nullptr;
}

SurfaceFrame SurfaceRenderer::begin_frame(rhi::RenderTarget& destination) {
    SurfaceFrame frame;
    if (in_frame_) {
        // One open frame per renderer: a second begin_frame while one is open would leak the first.
        // It counts as a skipped attempt, like every other refusal.
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::SlotBusy;
        return frame;
    }
    if (device_->status() != rhi::DeviceStatus::Ready) {
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::DeviceLost;
        return frame;
    }
    if (!surface_.valid()) {
        // The owner is gone or revoked: a state, not an error, and the reason a stale renderer is
        // safe to keep (see hal::NativeSurface).
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::NoSurface;
        return frame;
    }
    // The composite is a format-preserving copy, so a destination in another format would be a
    // silent colour change, and a destination of another extent cannot take the frame 1:1. Report a
    // typed state rather than throwing on the UI thread mid-frame.
    if (destination.extent().width != extent().width ||
        destination.extent().height != extent().height || destination.format() != config_.format) {
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::IncompatibleDestination;
        return frame;
    }
    if (!prepare_target()) {
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::DeviceLost;
        return frame;
    }
    rhi::CommandList* commands = free_slot();
    if (commands == nullptr) {
        ++skipped_frames_;
        frame.status = SurfaceCompositeStatus::SlotBusy;
        return frame;
    }
    frame_commands_ = commands;
    frame_destination_ = &destination;
    in_frame_ = true;
    frame.commands = commands;
    frame.target = target_.get();
    frame.status = SurfaceCompositeStatus::Composited;
    return frame;
}

SurfaceCompositeStatus SurfaceRenderer::end_frame() {
    if (!in_frame_ || frame_commands_ == nullptr || frame_destination_ == nullptr) {
        return SurfaceCompositeStatus::SlotBusy;
    }
    rhi::CommandList& commands = *frame_commands_;
    rhi::RenderTarget& destination = *frame_destination_;
    frame_commands_ = nullptr;
    frame_destination_ = nullptr;
    in_frame_ = false;

    // The offscreen composition of ADR-050 section 7 item 6: the target the surface's renderer drew
    // into is composited into the surface's presentation image 1:1. A region copy keeps the pixels
    // exact (no filtering) and is what a golden can compare byte for byte; RHI 1.1 makes a
    // swapchain image neither a copy source nor a destination, so a presentable destination uses
    // the resample blit the adaptive render scale already runs - at scale 1.0 a 1:1 blit, exact on
    // every backend whose present path supports it.
    if (destination.presentable()) {
        commands.blit_target(*target_, destination, rhi::Filter::Nearest);
    } else {
        commands.copy_target_region(
            *target_, rhi::Scissor{0, 0, target_->extent().width, target_->extent().height},
            destination, rhi::Offset2D{0, 0});
    }
    commands.submit_async();
    ++composited_frames_;
    return SurfaceCompositeStatus::Composited;
}

} // namespace tessera::rendering
