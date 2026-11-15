// SPDX-License-Identifier: MIT

#include "scene_frame.hpp"
#include "scene_prepare.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/scene_style.hpp"

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
// The four skipped kinds are each a missing *input*, not a missing capability (see the header): no
// font bytes, no AssetBundle. Counting them is what keeps "the canvas is empty" attributable.
class CountingSink final : public paint::PaintSink {
public:
    CountingSink(paint::PaintSink& inner, SceneFrameResult& stats)
        : inner_(&inner),
          stats_(&stats) {}

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

    // No font in the preview module: the node's box was already painted, the run is counted.
    void draw_text(const paint::TextDraw&) override {
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
};

// One device, one frame: everything the renderer borrows is created here and released with it. A
// failed creation is a value, not an exception, because the browser host reports it on the page.
} // namespace

rhi::Color scene_frame_clear_color() noexcept {
    // The app window background (`detail::frame_clear_color(false)` of
    // src/app/window_internal.hpp). A preview that cleared to black would differ from the native
    // window everywhere the stylesheet paints nothing, which is a fake parity failure rather than a
    // real one.
    return rhi::rgba8(11, 18, 32);
}

rhi::PipelineDesc scene_frame_pipeline_desc(rhi::TextureFormat format, std::uint32_t sample_count) {
    rhi::PipelineDesc desc;
    desc.vertex_layout.stride = gpu::kVertex2DStride;
    desc.vertex_layout.attributes = gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = gpu::kVertex2DAttributeCount;
    desc.color_format = format;
    desc.sample_count = sample_count;
    desc.blend = rhi::BlendMode::Alpha;
    desc.push_constant_size = sizeof(gpu::Batched2DPushConstants);
    // ADR-049: the logical name, not SPIR-V. Vulkan resolves it to its compiled program and WebGPU
    // to the embedded WGSL twin, so one description serves both lanes. The SPIR-V fields stay empty
    // on purpose: this description must be creatable by a backend with no GLSL output at all.
    desc.shader_id = gpu::kBatched2dShaderId;
    return desc;
}

SceneFrameResult scene_frame_paint(const PreparedScene& scene, rhi::Device& device,
                                   rhi::RenderTarget& target, const rhi::RenderTargetDesc& desc,
                                   const rhi::PipelineDesc& pipeline) {
    SceneFrameResult result;
    if (scene.error.empty() == false || scene.root == nullptr || !scene.sheet.has_value()) {
        result.error = scene.error.empty() ? "the source produced no scene" : scene.error;
        return result;
    }

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
    ui::style::SceneStyleTree style_tree(*scene.root);
    paint::SceneStyleProvider styles(*scene.sheet, style_tree);
    paint::Batched2DSink batched(renderer);
    CountingSink sink(batched, result);

    try {
        renderer.begin(*commands, target, desc);
        const paint::PaintStats painted = paint::paint_scene(*scene.root, styles, sink);
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
    } catch (const std::exception& failure) {
        result.error = std::string("scene_frame_paint: ") + failure.what();
        return result;
    }
    return result;
}

SceneFrameResult scene_frame_render(std::string_view text, const ScenePreviewOptions& options,
                                    rhi::Device& device, rhi::Extent2D extent,
                                    std::string_view component, const rhi::PipelineDesc& pipeline) {
    SceneFrameResult result;
    if (extent.width == 0 || extent.height == 0) {
        result.error = "scene_frame_render: the target extent must be positive";
        return result;
    }

    PreparedScene scene = prepare_scene(text, options, component);
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
    SceneFrameResult painted = scene_frame_paint(scene, device, *target, target_desc, pipeline);
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
