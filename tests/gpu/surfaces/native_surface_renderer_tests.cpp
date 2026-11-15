// SPDX-License-Identifier: MIT
//
// AF6 (issue #178, ADR-050 section 7): `SurfaceRenderer` on the null backend, which records the
// command stream instead of moving pixels. This is the structural half of the acceptance and it
// runs on every lane, with or without a GPU.
//
// It pins the frame the ADR describes, in order:
//
//   begin_frame(destination) -> a slot and a target for the surface's own pixels
//   Batched2DRenderer draws into it (the caller owns the pass)
//   end_frame()              -> the pass closed, one composite into the destination, one async
//                               submit, and no wait anywhere
//
// and the states the type reports instead of throwing: no surface (the owner was destroyed or
// revoked it), an incompatible destination, a second frame while one is open, and `end_frame()`
// with no frame open.

#include "tessera/hal/surface.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/surface_renderer.hpp"
#include "tessera/rendering/null/null_device.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <memory>

namespace {

using tessera::hal::NativeSurface;
using tessera::hal::SurfaceDesc;
using tessera::hal::SurfaceLifetime;
using tessera::hal::WindowSystem;
using tessera::rendering::SurfaceCompositeStatus;
using tessera::rendering::SurfaceFrame;
using tessera::rendering::SurfaceRenderer;
using tessera::rendering::SurfaceRendererConfig;
using tessera::rendering::gpu::Batched2DConfig;
using tessera::rendering::gpu::Batched2DRenderer;
using tessera::rendering::null::NullCommandKind;
using tessera::rendering::null::NullCommandList;
using tessera::rendering::null::NullCommandStats;
using tessera::rendering::rhi::Device;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::RenderTarget;
using tessera::rendering::rhi::RenderTargetDesc;
using tessera::rendering::rhi::rgba8;
using tessera::rendering::rhi::TextureFormat;

constexpr Extent2D kExtent{48, 32};
constexpr std::uint64_t kSurfaceId = 21;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "native_surface_renderer: " << message << "\n";
        ++failures;
    }
    return condition;
}

SurfaceDesc offscreen_desc() {
    SurfaceDesc desc;
    desc.width_px = kExtent.width;
    desc.height_px = kExtent.height;
    desc.dpi_scale = 1.0f;
    desc.window_system = WindowSystem::NoWindow;
    return desc;
}

// The renderer hands out a list it created, so the test reads the null backend's record through it.
// The renderer keeps the slot alive, so the reference is valid until that slot is reused.
const NullCommandStats& stats_of(const SurfaceFrame& frame) {
    return static_cast<const NullCommandList*>(frame.commands)->stats();
}

std::unique_ptr<RenderTarget>
make_destination(Device& device, Extent2D extent,
                 TextureFormat format = TextureFormat::R8G8B8A8Unorm) {
    RenderTargetDesc desc;
    desc.extent = extent;
    desc.format = format;
    desc.clear_color = rgba8(0, 0, 0, 255);
    return device.create_render_target(desc);
}

void test_frame_records_the_composite() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    SurfaceLifetime owner;
    const NativeSurface surface = NativeSurface::borrow(owner, offscreen_desc(), {}, kSurfaceId);

    SurfaceRendererConfig config;
    config.clear_color = rgba8(11, 18, 32, 255);
    SurfaceRenderer renderer(*device, surface, config);
    expect(renderer.valid(), "a renderer over a live surface is valid");
    expect(!renderer.native_presentable(), "an offscreen surface has no native target");
    expect(renderer.extent().width == kExtent.width && renderer.extent().height == kExtent.height,
           "the renderer takes the surface's physical pixels");
    expect(renderer.surface_desc().window_system == WindowSystem::NoWindow,
           "the renderer reports the surface's family");

    std::unique_ptr<RenderTarget> destination = make_destination(*device, kExtent);
    const SurfaceFrame frame = renderer.begin_frame(*destination);
    if (!expect(static_cast<bool>(frame), "the first frame opens")) {
        return;
    }
    expect(frame.status == SurfaceCompositeStatus::Composited, "the first frame composites");
    expect(frame.target != nullptr && frame.commands != nullptr,
           "the frame borrows a target and a list");
    expect(frame.target->extent().width == kExtent.width &&
               frame.target->extent().height == kExtent.height,
           "the composited target is the surface's extent");
    expect(frame.target->format() == config.format, "the composited target uses the config format");

    // The caller owns the pass, which is what lets Batched2DRenderer draw a surface directly: the
    // descriptor it opens with comes from the renderer, so extent, format and clear cannot drift.
    const RenderTargetDesc desc = renderer.target_desc();
    const bool desc_ok =
        expect(desc.extent.width == kExtent.width && desc.extent.height == kExtent.height,
               "the target descriptor carries the surface's extent") &&
        expect(desc.format == config.format, "and its format") &&
        expect(desc.clear_color.a == config.clear_color.a,
               "the clear colour comes from the config") &&
        expect(desc.load_op == tessera::rendering::rhi::LoadOp::Clear, "the frame clears");
    expect(desc_ok, "the target descriptor is the one the caller opens the pass with");

    auto pipeline = device->create_pipeline(tessera::rendering::rhi::PipelineDesc{});
    std::unique_ptr<tessera::rendering::rhi::Texture> white =
        device->create_texture(tessera::rendering::rhi::TextureDesc{{1, 1}});

    Batched2DConfig batched_config;
    batched_config.enable_aa = false;
    batched_config.golden_mode = true;
    Batched2DRenderer batched(*device, *pipeline, *white, batched_config);
    batched.begin(*frame.commands, *frame.target, desc);
    batched.push_rect({0, 0, 16, 16}, rgba8(255, 0, 0));
    batched.push_rect({16, 0, 16, 16}, rgba8(0, 255, 0));
    batched.end();

    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited, "end_frame composites");
    expect(renderer.composited_frames() == 1, "the frame counts as composited");
    expect(renderer.skipped_frames() == 0, "no frame was skipped");

    const NullCommandStats& stats = stats_of(frame);
    bool order_ok =
        expect(stats.begin_render_passes == 1, "exactly one pass opened on the surface target") &&
        expect(stats.end_render_passes == 1, "the caller's pass closed before the composite") &&
        // Two rects queued in one batch are one draw of twelve vertices (six per quad);
        // the vertex count is what proves both reached the GPU.
        expect(stats.vertices == 12, "both rects reached the command stream as one batched draw") &&
        expect(stats.region_copies == 1,
               "the composite is exactly one region copy into the destination");
    if (!order_ok) {
        return;
    }
    // Order is the contract: the pass first, the composite immediately before the submit, and
    // nothing after it, because the composite is the frame's last recorded work.
    bool stream_ok =
        expect(!stats.command_order.empty() &&
                   stats.command_order.front() == NullCommandKind::BeginRenderPass,
               "the frame opens with the pass on the surface target") &&
        expect(stats.command_order.back() == NullCommandKind::Submit,
               "the composite is submitted last, asynchronously") &&
        expect(stats.command_order[stats.command_order.size() - 2] ==
                   NullCommandKind::CopyTargetRegion,
               "the composite immediately precedes the submit") &&
        expect(stats.last_render_pass_sample_count == 1, "the surface target is single-sampled");
    expect(stream_ok, "the recorded frame is the ADR's composite shape");

    expect(stats.target_region_copies[0].source_extent.width == kExtent.width &&
               stats.target_region_copies[0].source_extent.height == kExtent.height,
           "the copy reads the whole composited target");
    expect(stats.target_region_copies[0].destination_extent.width == kExtent.width,
           "the copy lands in the destination 1:1");
    expect(stats.target_region_copies[0].destination_offset.x == 0 &&
               stats.target_region_copies[0].destination_offset.y == 0,
           "the composite is placed at the destination origin");
}

void test_states_are_reported_not_thrown() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    SurfaceLifetime owner;
    const NativeSurface surface = NativeSurface::borrow(owner, offscreen_desc());

    SurfaceRenderer renderer(*device, surface);
    std::unique_ptr<RenderTarget> destination = make_destination(*device, kExtent);

    // end_frame with no open frame is a state, not an error.
    expect(renderer.end_frame() == SurfaceCompositeStatus::SlotBusy,
           "end_frame with no frame open reports SlotBusy");

    // A second begin_frame while one is open is refused rather than leaking the first.
    const SurfaceFrame first = renderer.begin_frame(*destination);
    expect(static_cast<bool>(first), "the frame opens");
    const SurfaceFrame second = renderer.begin_frame(*destination);
    expect(!static_cast<bool>(second), "a second frame while one is open is refused");
    expect(second.status == SurfaceCompositeStatus::SlotBusy, "the refusal is SlotBusy");
    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited,
           "the first frame still closes cleanly");

    // A destination of another extent cannot take the surface's pixels 1:1.
    std::unique_ptr<RenderTarget> wrong_size =
        make_destination(*device, Extent2D{kExtent.width + 8, kExtent.height});
    const SurfaceFrame mismatch = renderer.begin_frame(*wrong_size);
    expect(!static_cast<bool>(mismatch), "a mismatched destination is refused");
    expect(mismatch.status == SurfaceCompositeStatus::IncompatibleDestination,
           "the refusal names the destination as incompatible");
    expect(renderer.skipped_frames() >= 2, "refused attempts count as skipped frames");
    expect(renderer.composited_frames() == 1, "a refused frame composites nothing");

    // A default-constructed (invalid) surface is inert rather than fatal.
    SurfaceRenderer inert(*device, NativeSurface{});
    expect(!inert.valid(), "a renderer over no surface is invalid");
    const SurfaceFrame none = inert.begin_frame(*destination);
    expect(!static_cast<bool>(none), "an invalid surface records no frame");
    expect(none.status == SurfaceCompositeStatus::NoSurface, "the state is NoSurface");
}

// The ownership rule as the renderer sees it: the owner goes away and the renderer never touches
// the handle it borrowed. One layer up from tests/unit/hal/native_surface_tests.cpp, which proves
// the type itself; this proves the frame path honours it.
void test_destroyed_owner_stops_the_renderer() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    auto owner = std::make_unique<SurfaceLifetime>();
    const NativeSurface surface = NativeSurface::borrow(*owner, offscreen_desc());

    SurfaceRenderer renderer(*device, surface);
    std::unique_ptr<RenderTarget> destination = make_destination(*device, kExtent);
    expect(static_cast<bool>(renderer.begin_frame(*destination)), "a frame opens before the loss");
    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited, "and composites");

    owner.reset(); // the window is destroyed while the renderer is still alive

    expect(!renderer.valid(), "the renderer invalidates with its surface");
    const SurfaceFrame after = renderer.begin_frame(*destination);
    expect(!static_cast<bool>(after), "no frame is recorded after the owner is destroyed");
    expect(after.status == SurfaceCompositeStatus::NoSurface,
           "the renderer reports NoSurface rather than returning a released handle");
    expect(renderer.surface_desc().width_px == 0, "the description is gone too");
    expect(renderer.extent().width == 0, "the extent is gone too");

    // A caller keeps the object and rebinds it, which is what a host does across a window recreate.
    SurfaceLifetime replacement_owner;
    renderer.rebind(NativeSurface::borrow(replacement_owner, offscreen_desc()));
    expect(renderer.valid(), "rebinding to a fresh surface revives the renderer");
    const SurfaceFrame revived = renderer.begin_frame(*destination);
    expect(static_cast<bool>(revived), "the rebound renderer records a frame");
    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited, "and composites it");
}

void test_resize_rebinds_to_new_pixels() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    SurfaceLifetime owner;
    SurfaceDesc small = offscreen_desc();
    small.width_px = 16;
    small.height_px = 16;
    SurfaceRenderer renderer(*device, NativeSurface::borrow(owner, small));

    std::unique_ptr<RenderTarget> small_dest = make_destination(*device, Extent2D{16, 16});
    expect(static_cast<bool>(renderer.begin_frame(*small_dest)), "a 16x16 frame opens");
    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited, "and composites");

    SurfaceDesc large = offscreen_desc();
    large.width_px = 64;
    large.height_px = 48;
    renderer.rebind(NativeSurface::borrow(owner, large));
    expect(renderer.extent().width == 64, "the renderer follows the re-borrowed extent");

    std::unique_ptr<RenderTarget> large_dest = make_destination(*device, Extent2D{64, 48});
    const SurfaceFrame frame = renderer.begin_frame(*large_dest);
    if (!expect(static_cast<bool>(frame), "the resized frame opens")) {
        return;
    }
    expect(frame.target->extent().width == 64 && frame.target->extent().height == 48,
           "the composited target follows the resize");
    expect(frame.target->format() == renderer.config().format, "and keeps the config format");
    expect(renderer.end_frame() == SurfaceCompositeStatus::Composited, "and composites");
    expect(renderer.composited_frames() == 2, "both frames count");
}

void test_format_mismatch_is_refused() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    SurfaceLifetime owner;
    SurfaceRenderer renderer(*device, NativeSurface::borrow(owner, offscreen_desc()));
    // A destination in another format would silently recolour the surface.
    std::unique_ptr<RenderTarget> other_format =
        make_destination(*device, kExtent, TextureFormat::B8G8R8A8Unorm);
    const SurfaceFrame frame = renderer.begin_frame(*other_format);
    expect(!static_cast<bool>(frame), "a format mismatch is refused");
    expect(frame.status == SurfaceCompositeStatus::IncompatibleDestination,
           "the refusal is reported as IncompatibleDestination");
    expect(renderer.composited_frames() == 0, "nothing was submitted");
}

void test_zero_frames_in_flight_is_rejected() {
    std::unique_ptr<Device> device = tessera::rendering::null::create_null_device({});
    SurfaceLifetime owner;
    SurfaceRendererConfig config;
    config.frames_in_flight = 0;
    bool threw = false;
    try {
        const SurfaceRenderer renderer(*device, NativeSurface::borrow(owner, offscreen_desc()),
                                       config);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    expect(threw, "a zero frames-in-flight configuration is rejected at construction");
}

} // namespace

int main() {
    test_frame_records_the_composite();
    test_states_are_reported_not_thrown();
    test_destroyed_owner_stops_the_renderer();
    test_resize_rebinds_to_new_pixels();
    test_format_mismatch_is_refused();
    test_zero_frames_in_flight_is_rejected();
    if (failures != 0) {
        std::cerr << "native_surface_renderer: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "native_surface_renderer: ok\n";
    return 0;
}
