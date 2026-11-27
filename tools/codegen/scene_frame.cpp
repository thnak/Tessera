// SPDX-License-Identifier: MIT

#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scale/render_target_extent.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace gpu = tessera::rendering::gpu;
namespace paint = tessera::ui::paint;

// Counting wrapper around the batched 2D sink: forwards every command that sink can express and
// counts the ones this preview has no input for. It wraps rather than replaces the sink, so the
// pixels come from the same `Batched2DSink` the native window driver and the golden harness use.
//
// The skipped kinds are each a missing *input*, not a missing capability (see the header): no
// AssetBundle for vectors and images, and - for text - no font, which is a property of this frame
// rather than of the preview: when the caller supplied a `PreviewTextStack` the run is forwarded to
// the real sink, which shapes and draws it through the production `TextRenderer`. Counting the
// remainder is what keeps "the canvas is empty" attributable.
class CountingSink final : public paint::PaintSink {
public:
    CountingSink(paint::PaintSink& inner, SceneFrameResult& stats, bool has_text)
        : inner_(&inner),
          stats_(&stats),
          has_text_(has_text) {}

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override {
        ++stats_->backgrounds;
        inner_->fill_rect(rect, color);
    }

    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override {
        ++stats_->backgrounds;
        inner_->fill_rounded_rect(rect, color, radius);
    }

    void fill_linear_gradient(const paint::GradientDraw& draw) override {
        inner_->fill_linear_gradient(draw);
    }
    void push_clip(const paint::RoundedClip& clip) override {
        inner_->push_clip(clip);
    }
    void pop_clip() override {
        inner_->pop_clip();
    }
    [[nodiscard]] bool supports_transform() const override {
        return inner_->supports_transform();
    }
    void push_transform(const rhi::Mat3& transform) override {
        inner_->push_transform(transform);
    }
    void pop_transform() override {
        inner_->pop_transform();
    }

    // With a font the run goes to the nested sink, which shapes it with `HarfBuzzShaper` and draws
    // it through `TextRenderer`'s bounded atlas - the production path, not a preview-only one.
    // Without one, the node's box was already painted and the run is counted.
    void draw_text(const paint::TextDraw& run) override {
        if (has_text_) {
            inner_->draw_text(run);
            return;
        }
        ++stats_->text_skipped;
    }
    void draw_vector(const paint::VectorDraw&) override {
        ++stats_->vectors_skipped;
    }
    void draw_image(const paint::ImageDraw&) override {
        ++stats_->images_skipped;
    }
    [[nodiscard]] bool supports_backdrop() const override {
        return inner_->supports_backdrop();
    }
    void draw_backdrop(const paint::BackdropDraw& draw) override {
        if (!inner_->supports_backdrop()) {
            ++stats_->backdrops_skipped;
            return;
        }
        inner_->draw_backdrop(draw);
    }

private:
    paint::PaintSink* inner_ = nullptr;
    SceneFrameResult* stats_ = nullptr;
    // A property of the frame, not of the sink: whether this call was given a font to draw with.
    bool has_text_ = false;
};

// One device, one frame: everything the renderer borrows is created here and released with it. A
// failed creation is a value, not an exception, because the browser host reports it on the page.
} // namespace

rhi::Color scene_frame_clear_color() noexcept {
    // The canvas colour (`detail::frame_clear_color(false)` of src/app/window_internal.hpp): white,
    // as a browser's default canvas. A preview that cleared to anything else would differ from the
    // native window everywhere the stylesheet paints nothing, a fake parity failure.
    return rhi::rgba8(255, 255, 255);
}

rhi::PipelineDesc scene_frame_pipeline_desc(rhi::TextureFormat format, std::uint32_t sample_count) {
    rhi::PipelineDesc desc;
    desc.vertex_layout.stride = gpu::kVertex2DStride;
    desc.vertex_layout.attributes = gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = gpu::kVertex2DAttributeCount;
    desc.color_format = format;
    desc.sample_count = sample_count;
    desc.blend = rhi::BlendMode::Alpha;
    // The same push-constant range and descriptor-set layout the Vulkan batched2d pipeline uses, so
    // a preview frame and a window frame record the same commands. Since ADR-068 slice 3 the block
    // is 16 bytes and the clip records live in a storage buffer, and the range is widened to the
    // glass block's size so the two pipelines the renderer interleaves share one layout.
    desc.push_constant_size = static_cast<std::uint32_t>(
        std::max(sizeof(gpu::Batched2DPushConstants), sizeof(gpu::GlassPushConstants)));
    desc.storage_buffer = true;
    // ADR-049: the logical name, not SPIR-V. Vulkan resolves it to its compiled program and WebGPU
    // to the embedded WGSL twin, so one description serves both lanes. The SPIR-V fields stay empty
    // on purpose: this description must be creatable by a backend with no GLSL output at all.
    desc.shader_id = gpu::kBatched2dShaderId;
    return desc;
}

SceneFrameResult scene_frame_paint_tree(const ui::Node& root,
                                        const ui::style::CompiledStyleSheet& sheet,
                                        rhi::Device& device, rhi::RenderTarget& target,
                                        const rhi::RenderTargetDesc& desc,
                                        const rhi::PipelineDesc& pipeline, PreviewTextStack* text,
                                        float device_scale) {
    SceneFrameResult result;
    // The device scale is the frame's, not the layout's (issue #599): the tree is laid out in CSS
    // px and the target is the device-pixel grid, so a DPR-2 canvas needs every position doubled or
    // the scene paints into the top-left quarter of its surface. `content_scale_for` is what the
    // app's frame uses, so a bad scale degrades to 1 instead of collapsing or mirroring the frame.
    const ui::scale::ContentScale scale = ui::scale::content_scale_for(device_scale);
    // A pipeline is created for the target's format and sample count: a mismatch is a pipeline the
    // renderer refuses anyway, so it is settled here while the description is in hand.
    rhi::PipelineDesc effective = pipeline;
    if (effective.vertex_layout.stride == 0) {
        effective = scene_frame_pipeline_desc(desc.format, desc.sample_count);
    }
    std::unique_ptr<rhi::Pipeline> created = device.create_pipeline(effective);
    // Opaque white: the batched renderer's "this draw samples no texture" fill.
    const std::uint32_t white_pixel = 0xFFFFFFFFu;
    rhi::TextureDesc white_desc;
    white_desc.extent = {1, 1};
    white_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    white_desc.pixels = &white_pixel;
    white_desc.row_pitch = 4;
    std::unique_ptr<rhi::Texture> white = device.create_texture(white_desc);
    std::unique_ptr<rhi::CommandList> commands = device.create_command_list();
    if (created == nullptr || white == nullptr || commands == nullptr) {
        result.error = "scene_frame_paint: the device could not create the frame's objects";
        return result;
    }

    // AA off and golden mode: hard edges and exact bytes, the configuration the cross-backend
    // parity harness renders its reference with (tests/gpu/backends/parity_render.cpp). A preview
    // that antialiased differently from the reference would fail the tolerance for a reason that is
    // not a regression.
    gpu::Batched2DRenderer renderer(device, *created, *white,
                                    gpu::Batched2DConfig{4096, false, true});
    // Style must come from the sheet the layout resolved against, or a painted node could have a
    // different size than the box it was laid out in.
    ui::style::SceneStyleTree style_tree(root);
    paint::SceneStyleProvider styles(sheet, style_tree);
    // The font, when the caller has one: the production shaper and atlas renderer, borrowed for the
    // duration of the frame. Without it the sink draws boxes only and every run is counted, which
    // is the documented degraded state.
    paint::Batched2DSink batched(renderer, text != nullptr ? &text->renderer() : nullptr,
                                 text != nullptr ? &text->shaper() : nullptr);
    CountingSink sink(batched, result, text != nullptr);

    try {
        renderer.begin(*commands, target, desc);
        // **The device scale is the frame's, not the layout's** (issue #599): the tree is laid out
        // in CSS px and the target is the device-pixel grid, so the paint side has to compose the
        // two, exactly as the app's frame does (`apply_frame_transform`, src/app/window_aa.cpp).
        // The preview used to skip this, so a DPR-2 canvas painted the scene 1:1 into the top-left
        // quarter of its surface. At 1.0 the renderer's begin() already left the transform at
        // identity, so the ordinary path records the same commands it always did.
        // At 1.0 the renderer's begin() already left the transform at identity, and setting it
        // would record no different commands; skipping the call keeps the ordinary path byte-
        // identical to every frame recorded before this was applied.
        if (!scale.is_identity()) {
            renderer.set_content_scale(scale.x, scale.y);
        }
        if (text != nullptr) {
            // Once per frame, before any draw: the atlas pins the pages this frame references so a
            // genuine overflow is reported as a drop instead of silently evicting ink mid-frame.
            batched.begin_text_frame();
        }
        // The font-free path measures text with the deterministic stub, exactly as it always has; a
        // real face measures through the paint metrics source the stack exposes, so a wrapped
        // line's box matches the advances the shaper will produce.
        paint::PaintContext context;
        if (text != nullptr) {
            context.text_metrics_source = &text->paint_metrics();
        }
        // Border snapping and the vector raster extent follow the same scale the geometry does, so
        // the three cannot disagree about the device (src/app/window_frame.cpp sets the pair
        // together for the same reason).
        context.device_scale = scale.x;
        batched.set_vector_device_scale(scale.x);
        // The app's page colour fills the whole frame, as a browser's root background fills the
        // canvas.
        context.canvas = paint::canvas_background(root, styles);
        const paint::PaintStats painted = paint::paint_scene(root, styles, sink, context);
        renderer.end();
        // `submit()` blocks, which is correct natively and forbidden in a browser; the WebGPU
        // backend throws there by design, so the async path is the host's choice, not this
        // driver's.
        if (device.backend() == rhi::Backend::WebGpu) {
            commands->submit_async();
        } else {
            commands->submit();
        }
        result.nodes = painted.nodes;
        result.borders = painted.borders;
        result.texts = painted.texts;
        result.draw_calls = renderer.draw_calls();
        result.vertex_count = renderer.vertex_count();
        if (text != nullptr) {
            // Sampled after the frame: the atlas's drop counter is cumulative, and this frame is
            // the one whose draws could have added to it.
            result.glyphs_dropped = static_cast<std::size_t>(text->counters().glyphs_dropped);
        }
    } catch (const std::exception& failure) {
        result.error = std::string("scene_frame_paint: ") + failure.what();
        return result;
    }
    return result;
}

SceneFrameResult scene_frame_paint(const PreparedScene& scene, rhi::Device& device,
                                   rhi::RenderTarget& target, const rhi::RenderTargetDesc& desc,
                                   const rhi::PipelineDesc& pipeline, PreviewTextStack* text,
                                   float device_scale) {
    if (scene.error.empty() == false || scene.root == nullptr || !scene.sheet.has_value()) {
        SceneFrameResult result;
        result.error = scene.error.empty() ? "the source produced no scene" : scene.error;
        return result;
    }
    return scene_frame_paint_tree(*scene.root, *scene.sheet, device, target, desc, pipeline, text,
                                  device_scale);
}

SceneFrameResult scene_frame_render(std::string_view text, const ScenePreviewOptions& options,
                                    rhi::Device& device, rhi::Extent2D extent,
                                    std::string_view component, const rhi::PipelineDesc& pipeline,
                                    PreviewTextStack* font) {
    SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "scene_frame_render: the target extent must be positive";
        return result;
    }

    // The measurer decides the layout of every text box, so it must be the same face the frame will
    // paint with - or, with no font, the deterministic stub that every font-free golden was
    // recorded against.
    PreparedScene scene =
        prepare_scene(text, options, component, font != nullptr ? font->measurer() : nullptr);
    if (!scene.error.empty() || scene.root == nullptr) {
        result.error = scene.error.empty() ? "the source produced no scene" : scene.error;
        return result;
    }

    rhi::RenderTargetDesc target_desc;
    target_desc.extent = extent;
    target_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    target_desc.clear_color = scene_frame_clear_color();
    target_desc.load_op = rhi::LoadOp::Clear;
    target_desc.store_op = rhi::StoreOp::Store;

    std::unique_ptr<rhi::RenderTarget> target = device.create_render_target(target_desc);
    if (target == nullptr) {
        result.error = "scene_frame_render: create_render_target returned null";
        return result;
    }

    // Paint through the shared path, then read back.
    SceneFrameResult painted =
        scene_frame_paint(scene, device, *target, target_desc, pipeline, font, options.dpi_scale);
    if (!painted.error.empty()) {
        return painted;
    }
    result = std::move(painted);

    // Readback needs a synchronous map, which a browser does not have (the WebGPU backend's
    // `Buffer::read` throws by design). The command stream is recorded and the counters are valid,
    // so a browser caller gates on those; `pixels_available` says the image is absent instead of
    // handing back a zeroed one.
    if (device.backend() == rhi::Backend::WebGpu) {
        result.pixels_available = false;
        return result;
    }

    const std::size_t bytes =
        static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) * 4;
    rhi::BufferDesc readback_desc;
    readback_desc.size = bytes;
    readback_desc.usage = rhi::BufferUsage::Readback;
    std::unique_ptr<rhi::Buffer> readback = device.create_buffer(readback_desc);
    std::unique_ptr<rhi::CommandList> copy = device.create_command_list();
    if (readback == nullptr || copy == nullptr) {
        result.error = "scene_frame_render: the readback objects could not be created";
        return result;
    }
    try {
        copy->copy_target_to_buffer(*target, *readback);
        copy->submit();
        result.frame.width = extent.width;
        result.frame.height = extent.height;
        result.frame.rgba.resize(bytes);
        readback->read(result.frame.rgba.data(), result.frame.rgba.size());
    } catch (const std::exception& failure) {
        result.error = std::string("scene_frame_render: readback failed: ") + failure.what();
        result.frame = SceneFrame{};
        return result;
    }
    result.pixels_available = true;
    return result;
}

} // namespace tessera::compiler::codegen::cli
