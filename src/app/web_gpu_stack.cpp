// SPDX-License-Identifier: MIT

#include "tessera/hal/window.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/gpu/glass_blur.hpp"
#include "tessera/rendering/gpu/shader_ids.hpp"
#include "tessera/rendering/gpu/vertex2d.hpp"
#include "tessera/rendering/webgpu/path_raster_pipeline.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "text_stack.hpp"
#include "web_canvas_surface.hpp"
#include "web_gpu_stack.hpp"
#include "window_config.hpp"
#include "window_vector.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace tessera::app {

namespace {

namespace rhi = rendering::rhi;
namespace gpu = rendering::gpu;

// The same two shaders the Vulkan pipelines name, resolved to their WGSL twins by id (ADR-049). The
// push-constant size is the larger of the two blocks so every pipeline shares one layout.
constexpr std::uint32_t push_size() {
    return static_cast<std::uint32_t>(
        std::max(sizeof(gpu::Batched2DPushConstants), sizeof(gpu::GlassPushConstants)));
}

rhi::PipelineDesc batched2d_desc(rhi::TextureFormat format) {
    rhi::PipelineDesc desc;
    desc.vertex_layout.stride = gpu::kVertex2DStride;
    desc.vertex_layout.attributes = gpu::kVertex2DAttributes;
    desc.vertex_layout.attribute_count = gpu::kVertex2DAttributeCount;
    desc.color_format = format;
    desc.sample_count = 1;
    desc.blend = rhi::BlendMode::Alpha;
    desc.push_constant_size = push_size();
    desc.storage_buffer = true;
    desc.shader_id = gpu::kBatched2dShaderId;
    return desc;
}

// The glass passes draw a fullscreen triangle: no vertex buffer, one sampled target.
rhi::PipelineDesc glass_desc(rhi::TextureFormat format, rhi::BlendMode blend) {
    rhi::PipelineDesc desc;
    desc.color_format = format;
    desc.blend = blend;
    desc.push_constant_size = push_size();
    desc.storage_buffer = true;
    desc.shader_id = gpu::kGlassBlurShaderId;
    return desc;
}

// Optional pipelines only cost a feature (a mask mode), never the app.
std::unique_ptr<rhi::Pipeline> optional_pipeline(rhi::Device& device, rhi::PipelineDesc desc) {
    try {
        return device.create_pipeline(desc);
    } catch (const std::exception&) {
        return nullptr;
    }
}

} // namespace

struct WebGpuStack::Impl {
    // A WebGPU command list is single-use and cannot be waited on, so submitted lists are kept
    // until the GPU reports them complete and at most kMaxInFlight are outstanding.
    static constexpr std::size_t kMaxInFlight = 2;

    std::unique_ptr<hal::web::WebCanvasSurface> surface;
    std::unique_ptr<rhi::Pipeline> pipeline;
    std::unique_ptr<rhi::Pipeline> glass_blur;
    std::unique_ptr<rhi::Pipeline> glass_composite;
    std::unique_ptr<rhi::Pipeline> glass_mask;
    std::unique_ptr<rhi::Pipeline> glass_mask_out;
    std::unique_ptr<rhi::Pipeline> glass_mask_xor;
    std::unique_ptr<rhi::Texture> white;
    std::unique_ptr<gpu::Batched2DRenderer> renderer;
    std::unique_ptr<detail::TextStack> text;
    std::unique_ptr<detail::VectorStack> vectors;
    std::unique_ptr<ui::paint::Batched2DSink> sink;
    std::vector<std::unique_ptr<rhi::CommandList>> in_flight;
    hal::web::WebSurfaceExtent extent;
    float device_scale = 1.0F;
    bool failed = false;

    // Reverse dependency order: the sink borrows the renderer, the text stack and the vector
    // caches; those borrow the device the surface owns. A defaulted destructor would release them
    // in declaration order.
    ~Impl() {
        in_flight.clear();
        sink.reset();
        vectors.reset();
        text.reset();
        renderer.reset();
        white.reset();
        glass_mask_xor.reset();
        glass_mask_out.reset();
        glass_mask.reset();
        glass_composite.reset();
        glass_blur.reset();
        pipeline.reset();
        surface.reset();
    }
};

WebGpuStack::WebGpuStack(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
WebGpuStack::~WebGpuStack() = default;

std::unique_ptr<WebGpuStack> WebGpuStack::create(const WebAppOptions& options) {
    auto impl = std::make_unique<Impl>();
    try {
        impl->surface = hal::web::WebCanvasSurface::create(options.canvas_selector.c_str());
        if (impl->surface == nullptr) {
            return nullptr;
        }
        rhi::Device& device = impl->surface->device();
        const rhi::TextureFormat format = impl->surface->format();
        impl->pipeline = device.create_pipeline(batched2d_desc(format));
        impl->glass_blur = device.create_pipeline(glass_desc(format, rhi::BlendMode::None));
        impl->glass_composite = device.create_pipeline(glass_desc(format, rhi::BlendMode::Alpha));
        impl->glass_mask =
            optional_pipeline(device, glass_desc(format, rhi::BlendMode::DestinationIn));
        impl->glass_mask_out =
            optional_pipeline(device, glass_desc(format, rhi::BlendMode::SourceOut));
        impl->glass_mask_xor = optional_pipeline(device, glass_desc(format, rhi::BlendMode::Xor));
        const std::uint32_t white_pixel = 0xFFFFFFFFU;
        rhi::TextureDesc white_desc;
        white_desc.extent = {1, 1};
        white_desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
        white_desc.pixels = &white_pixel;
        white_desc.row_pitch = 4;
        impl->white = device.create_texture(white_desc);
        if (impl->pipeline == nullptr || impl->white == nullptr) {
            return nullptr;
        }
        gpu::Batched2DConfig config;
        config.frames_in_flight = static_cast<std::uint32_t>(Impl::kMaxInFlight);
        impl->renderer = std::make_unique<gpu::Batched2DRenderer>(
            device, *impl->pipeline, *impl->white, config,
            gpu::GlassPipelines{impl->glass_blur.get(), impl->glass_composite.get(),
                                impl->glass_mask.get(), impl->glass_mask_out.get(),
                                impl->glass_mask_xor.get()});
        if (options.font_paths.empty()) {
            return nullptr;
        }
        // One thread: the scale step runs synchronously (ADR-046 item 6).
        impl->text = std::make_unique<detail::TextStack>(
            device, options.font_paths.front(),
            std::vector<std::string>(options.font_paths.begin() + 1, options.font_paths.end()),
            false);
        // The WGSL path-raster programs register once; the GPU rasterizer falls back to ThorVG
        // when they are missing.
        const bool compute = rendering::webgpu::register_path_raster_shaders(device);
        impl->vectors = std::make_unique<detail::VectorStack>(
            device, format, options.vector_assets, nullptr,
            rendering::webgpu::path_raster_pipeline_descs(), compute);
        impl->sink = std::make_unique<ui::paint::Batched2DSink>(
            *impl->renderer, impl->text->renderer.get(), impl->text->shaper.get(),
            impl->vectors->available() ? impl->vectors->config(1.0F)
                                       : ui::paint::VectorPaintConfig{});
        impl->in_flight.reserve(Impl::kMaxInFlight);
    } catch (const std::exception& failure) {
        std::fprintf(stderr, "web app: no renderer: %s\n", failure.what());
        return nullptr;
    }
    return std::unique_ptr<WebGpuStack>(new WebGpuStack(std::move(impl)));
}

std::shared_ptr<const ui::text::TextMeasurer> WebGpuStack::measurer() const {
    return impl_->text->measurer;
}

bool WebGpuStack::has_capacity() {
    std::erase_if(impl_->in_flight, [](const auto& list) { return list->is_complete(); });
    return impl_->in_flight.size() < Impl::kMaxInFlight;
}

void WebGpuStack::set_device_scale(float device_scale) {
    impl_->device_scale = device_scale > 0.0F ? device_scale : 1.0F;
    hal::WindowMetrics metrics;
    metrics.dpi_scale = impl_->device_scale;
    WindowAaOptions options;
    options.lcd_supported = false;
    impl_->text->apply_policy(window_aa_policy(metrics, options));
}

bool WebGpuStack::failed() const {
    return impl_->failed;
}

hal::web::WebSurfaceExtent WebGpuStack::last_extent() const {
    return impl_->extent;
}

bool WebGpuStack::paint(AppHost& host, const hal::web::WebSurfaceMetrics& metrics) {
    Impl& s = *impl_;
    try {
        const hal::web::WebSurfaceExtent extent = hal::web::backing_extent(metrics);
        if (!s.surface->configure(extent)) {
            return false;
        }
        std::unique_ptr<rhi::RenderTarget> target = s.surface->acquire();
        if (target == nullptr) {
            return false;
        }
        s.extent = extent;
        rhi::RenderTargetDesc desc;
        desc.extent = {extent.width, extent.height};
        desc.format = s.surface->format();
        desc.clear_color = rhi::rgba8(255, 255, 255);
        desc.load_op = rhi::LoadOp::Clear;
        desc.store_op = rhi::StoreOp::Store;
        std::unique_ptr<rhi::CommandList> commands = s.surface->device().create_command_list();
        if (commands == nullptr) {
            return false;
        }
        s.renderer->begin(*commands, *target, desc);
        // CSS pixels in, device pixels out: the content scale the native window frame applies.
        if (s.device_scale != 1.0F) {
            s.renderer->set_content_scale(s.device_scale, s.device_scale);
        }
        s.sink->begin_text_frame();
        s.sink->set_vector_device_scale(s.device_scale);
        ui::paint::PaintContext context;
        context.text_metrics_source = s.text->paint_metrics.get();
        context.device_scale = s.device_scale;
        host.paint_scene(*s.sink, context);
        // The last SVG acquire of the frame has happened: submit the queued GPU rasters ahead of
        // the draw list that samples their textures.
        s.vectors->flush();
        s.renderer->end();
        commands->submit_async();
        s.in_flight.push_back(std::move(commands));
        // Only this frame is outstanding when every older one has completed, which is when a
        // texture evicted during the frame may be destroyed.
        s.vectors->end_frame(s.in_flight.size() == 1);
        return true;
    } catch (const std::exception& failure) {
        // Never throw across the FFI boundary (ADR-025): report once and stop painting.
        std::fprintf(stderr, "web app: frame failed: %s\n", failure.what());
        s.failed = true;
        return false;
    }
}

} // namespace tessera::app
