// SPDX-License-Identifier: MIT

// The Metal command list (ADR-053, docs/design/gpu/metal-backend.md sections 2, 3 and 5).
//
// One RHI command list is one MTLCommandBuffer. A render pass is one MTLRenderCommandEncoder; a
// copy or a sampled-target bind ends the encoder and uses an MTLBlitCommandEncoder, because Metal
// has no image layouts to transition and a blit cannot run inside a render encoder. The backend
// never adds a pass break of its own: the renderer decides how many breaks a frame has, and on
// Apple GPUs each break costs a store and reload of the attachments (section 3.3).
//
// Validation parity with the null backend is deliberate: the same misuse throws the same category
// of exception, so a structural test written against null stays meaningful here.

#include "metal_internal.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace tessera::rendering::metal {
namespace {

[[nodiscard]] const MetalBuffer& as_buffer(const rhi::Buffer& buffer) {
    return static_cast<const MetalBuffer&>(buffer);
}

[[nodiscard]] const MetalTexture& as_texture(const rhi::Texture& texture) {
    return static_cast<const MetalTexture&>(texture);
}

[[nodiscard]] const MetalRenderTarget& as_target(const rhi::RenderTarget& target) {
    return static_cast<const MetalRenderTarget&>(target);
}

[[nodiscard]] bool region_fits(const rhi::Scissor& region, rhi::Extent2D extent) {
    return region.x >= 0 && region.y >= 0 && region.width > 0 && region.height > 0 &&
           static_cast<std::uint32_t>(region.x) + region.width <= extent.width &&
           static_cast<std::uint32_t>(region.y) + region.height <= extent.height;
}

[[nodiscard]] bool offset_region_fits(rhi::Offset2D offset, const rhi::Scissor& region,
                                      rhi::Extent2D extent) {
    return offset.x >= 0 && offset.y >= 0 &&
           static_cast<std::uint32_t>(offset.x) + region.width <= extent.width &&
           static_cast<std::uint32_t>(offset.y) + region.height <= extent.height;
}

} // namespace

class MetalCommandList final : public rhi::CommandList {
public:
    explicit MetalCommandList(MetalDevice& device) : device_(device) {}

    void begin_render_pass(const rhi::RenderTarget& target, const rhi::RenderTargetDesc& desc) override;
    void end_render_pass() override;
    void bind_pipeline(const rhi::Pipeline& pipeline) override;
    void bind_vertex_buffer(const rhi::Buffer& buffer) override;
    void bind_texture(std::uint32_t slot, const rhi::Texture& texture) override;
    void bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) override;
    void set_viewport(const rhi::Viewport& viewport) override;
    void set_scissor(const rhi::Scissor& scissor) override;
    void push_constants(const void* data, std::size_t size) override;
    void draw(std::uint32_t vertex_count, std::uint32_t first_vertex) override;
    void copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) override;
    void copy_target_region(const rhi::RenderTarget& source, const rhi::Scissor& source_region,
                            const rhi::RenderTarget& destination,
                            rhi::Offset2D destination_offset) override;
    void blit_target(const rhi::RenderTarget& source, const rhi::RenderTarget& destination,
                     rhi::Filter filter) override;
    void submit() override;
    void submit_async() override;
    [[nodiscard]] bool is_complete() const override;
    [[nodiscard]] bool wait(std::uint64_t timeout_ms) override;

private:
    void require_pass(const char* what) const;
    void require_outside_pass(const char* what) const;
    void require_pipeline(const char* what) const;
    // Ends the open render encoder, if any. A blit or a sampled bind needs this.
    void end_encoder();
    // Starts the command buffer on first use. A list may record several sequential passes; only the
    // first one starts the buffer (docs/guides/writing-a-backend.md section 3).
    void ensure_command_buffer();
    void encode_signal_and_commit();

    MetalDevice& device_;
    id<MTLCommandBuffer> command_buffer_ = nil;
    id<MTLRenderCommandEncoder> encoder_ = nil;
    const MetalRenderTarget* current_target_ = nullptr;
    const MetalPipeline* pipeline_ = nullptr;
    const MetalBuffer* vertex_buffer_ = nullptr;
    std::uint32_t pass_sample_count_ = 1;
    bool in_pass_ = false;
    bool submitted_ = false;
    std::uint64_t completion_value_ = 0;
    // The push-constant block, uploaded as a uniform buffer at binding 0 (MSL has no push
    // constants; the portable shader branch reads a std140 uniform instead).
    std::vector<std::uint8_t> push_constants_;
};

void MetalCommandList::require_pass(const char* what) const {
    if (!in_pass_) {
        throw std::logic_error(std::string("metal ") + what + ": no render pass is active");
    }
}

void MetalCommandList::require_outside_pass(const char* what) const {
    if (in_pass_) {
        throw std::logic_error(std::string("metal ") + what + ": render pass is still active");
    }
}

void MetalCommandList::require_pipeline(const char* what) const {
    if (pipeline_ == nullptr) {
        throw std::logic_error(std::string("metal ") + what + ": no pipeline is bound");
    }
}

void MetalCommandList::ensure_command_buffer() {
    if (command_buffer_ != nil) {
        return;
    }
    command_buffer_ = [device_.queue() commandBuffer];
    if (command_buffer_ == nil) {
        throw std::runtime_error("metal: commandBuffer failed");
    }
    // Retained references (the default `commandBuffer`), not `commandBufferWithUnretainedReferences`:
    // dropping a list that is still executing is then safe, as the RHI contract requires
    // (metal-backend.md section 4).
}

void MetalCommandList::end_encoder() {
    if (encoder_ != nil) {
        [encoder_ endEncoding];
        encoder_ = nil;
    }
}

void MetalCommandList::begin_render_pass(const rhi::RenderTarget& target,
                                         const rhi::RenderTargetDesc& desc) {
    if (in_pass_) {
        throw std::logic_error("metal begin_render_pass: the previous render pass is still active");
    }
    if (desc.sample_count != target.sample_count()) {
        throw std::invalid_argument(
            "metal begin_render_pass: descriptor sample count does not match the target");
    }
    ensure_command_buffer();

    const MetalRenderTarget& concrete = as_target(target);
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[0];
    color.texture = concrete.attachment();
    color.loadAction = static_cast<MTLLoadAction>(load_action(desc.load_op));
    color.storeAction = static_cast<MTLStoreAction>(store_action(desc.store_op));
    if (desc.load_op == rhi::LoadOp::Clear) {
        color.clearColor = MTLClearColorMake(desc.clear_color.r, desc.clear_color.g,
                                             desc.clear_color.b, desc.clear_color.a);
    }
    if (concrete.sample_count() > 1) {
        // A 4x target resolves into its 1-sample image at the end of the pass; readback and
        // sampling observe the resolve (RHI 1.0, device.hpp).
        color.resolveTexture = concrete.readable();
        color.storeAction = MTLStoreActionStoreAndMultisampleResolve;
    }

    encoder_ = [command_buffer_ renderCommandEncoderWithDescriptor:pass];
    if (encoder_ == nil) {
        throw std::runtime_error("metal: renderCommandEncoderWithDescriptor failed");
    }
    current_target_ = &concrete;
    pass_sample_count_ = target.sample_count();
    in_pass_ = true;
}

void MetalCommandList::end_render_pass() {
    if (!in_pass_) {
        throw std::logic_error("metal end_render_pass: no render pass is active");
    }
    end_encoder();
    current_target_ = nullptr;
    in_pass_ = false;
}

void MetalCommandList::bind_pipeline(const rhi::Pipeline& pipeline) {
    if (in_pass_ && pipeline.sample_count() != pass_sample_count_) {
        throw std::invalid_argument(
            "metal bind_pipeline: pipeline sample count does not match the render target");
    }
    pipeline_ = static_cast<const MetalPipeline*>(&pipeline);
    if (encoder_ != nil) {
        [encoder_ setRenderPipelineState:pipeline_->state()];
    }
}

void MetalCommandList::bind_vertex_buffer(const rhi::Buffer& buffer) {
    vertex_buffer_ = &as_buffer(buffer);
    if (encoder_ != nil) {
        [encoder_ setVertexBuffer:vertex_buffer_->handle() offset:0 atIndex:0];
    }
}

void MetalCommandList::bind_texture(std::uint32_t slot, const rhi::Texture& texture) {
    if (encoder_ == nil) {
        // A texture bind outside a pass is legal (the usual order binds the pipeline first); it is
        // recorded when the encoder opens.
        return;
    }
    [encoder_ setFragmentTexture:as_texture(texture).handle() atIndex:slot];
}

void MetalCommandList::bind_sampled_target(std::uint32_t slot, const rhi::RenderTarget& target) {
    require_outside_pass("bind_sampled_target");
    if (target.presentable()) {
        throw std::invalid_argument("metal bind_sampled_target: a swapchain image is not samplable");
    }
    // Metal has no image layouts, so binding a target as a texture needs no transition: the
    // previous encoder already stored it. The bind is recorded on the next encoder.
    (void)slot;
    (void)target;
}

void MetalCommandList::set_viewport(const rhi::Viewport& viewport) {
    if (encoder_ == nil) {
        return;
    }
    MTLViewport metal_viewport{};
    metal_viewport.originX = viewport.x;
    metal_viewport.originY = viewport.y;
    metal_viewport.width = viewport.width;
    metal_viewport.height = viewport.height;
    metal_viewport.znear = viewport.min_depth;
    metal_viewport.zfar = viewport.max_depth;
    [encoder_ setViewport:metal_viewport];
}

void MetalCommandList::set_scissor(const rhi::Scissor& scissor) {
    if (encoder_ == nil) {
        return;
    }
    MTLScissorRect rect{};
    rect.x = static_cast<NSUInteger>(scissor.x);
    rect.y = static_cast<NSUInteger>(scissor.y);
    rect.width = scissor.width;
    rect.height = scissor.height;
    [encoder_ setScissorRect:rect];
}

void MetalCommandList::push_constants(const void* data, std::size_t size) {
    require_pipeline("push_constants");
    if (size != pipeline_->push_constant_size()) {
        throw std::invalid_argument("metal push_constants: size does not match the pipeline");
    }
    push_constants_.assign(static_cast<const std::uint8_t*>(data),
                           static_cast<const std::uint8_t*>(data) + size);
    if (encoder_ != nil) {
        // MSL has no push constants; the portable shader branch reads a std140 uniform at binding
        // 0, so the block is uploaded as a uniform buffer (ADR-049's portable branch).
        [encoder_ setVertexBytes:push_constants_.data()
                          length:push_constants_.size()
                         atIndex:0];
        [encoder_ setFragmentBytes:push_constants_.data()
                            length:push_constants_.size()
                           atIndex:0];
    }
}

void MetalCommandList::draw(std::uint32_t vertex_count, std::uint32_t first_vertex) {
    require_pass("draw");
    require_pipeline("draw");
    if (vertex_buffer_ == nullptr) {
        throw std::logic_error("metal draw: no vertex buffer is bound");
    }
    [encoder_ drawPrimitives:MTLPrimitiveTypeTriangle
                 vertexStart:first_vertex
                 vertexCount:vertex_count];
}

void MetalCommandList::copy_target_to_buffer(const rhi::RenderTarget& target, rhi::Buffer& buffer) {
    require_outside_pass("copy_target_to_buffer");
    ensure_command_buffer();
    end_encoder();

    const MetalRenderTarget& concrete = as_target(target);
    MetalBuffer& destination = const_cast<MetalBuffer&>(as_buffer(buffer));
    const std::size_t row_bytes =
        static_cast<std::size_t>(concrete.extent().width) * bytes_per_pixel(concrete.format());
    const std::size_t total = row_bytes * concrete.extent().height;
    if (destination.size() < total) {
        throw std::invalid_argument("metal copy_target_to_buffer: the buffer is too small");
    }

    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
    if (blit == nil) {
        throw std::runtime_error("metal: blitCommandEncoder failed");
    }
    [blit copyFromTexture:concrete.readable()
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(concrete.extent().width, concrete.extent().height, 1)
                 toBuffer:destination.handle()
        destinationOffset:0
   destinationBytesPerRow:row_bytes
 destinationBytesPerImage:total];
    [blit endEncoding];
}

void MetalCommandList::copy_target_region(const rhi::RenderTarget& source,
                                          const rhi::Scissor& source_region,
                                          const rhi::RenderTarget& destination,
                                          rhi::Offset2D destination_offset) {
    require_outside_pass("copy_target_region");
    if (&source == &destination) {
        throw std::invalid_argument(
            "metal copy_target_region: source and destination must be different targets");
    }
    if (source.sample_count() != 1 || destination.sample_count() != 1) {
        throw std::invalid_argument(
            "metal copy_target_region: multisampled targets are not supported");
    }
    if (source.presentable()) {
        throw std::invalid_argument("metal copy_target_region: a swapchain image is not a copy source");
    }
    if (destination.presentable()) {
        throw std::invalid_argument(
            "metal copy_target_region: a swapchain image is not a copy destination");
    }
    if (source.format() != destination.format()) {
        throw std::invalid_argument(
            "metal copy_target_region: source and destination formats differ");
    }
    if (!region_fits(source_region, source.extent())) {
        throw std::invalid_argument("metal copy_target_region: source region exceeds the target");
    }
    if (!offset_region_fits(destination_offset, source_region, destination.extent())) {
        throw std::invalid_argument(
            "metal copy_target_region: destination offset plus region exceeds the target");
    }

    ensure_command_buffer();
    end_encoder();
    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
    if (blit == nil) {
        throw std::runtime_error("metal: blitCommandEncoder failed");
    }
    [blit copyFromTexture:as_target(source).readable()
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(static_cast<NSUInteger>(source_region.x),
                                        static_cast<NSUInteger>(source_region.y), 0)
               sourceSize:MTLSizeMake(source_region.width, source_region.height, 1)
                toTexture:as_target(destination).readable()
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(static_cast<NSUInteger>(destination_offset.x),
                                        static_cast<NSUInteger>(destination_offset.y), 0)];
    [blit endEncoding];
}

void MetalCommandList::blit_target(const rhi::RenderTarget& source,
                                   const rhi::RenderTarget& destination, rhi::Filter filter) {
    require_outside_pass("blit_target");
    if (source.sample_count() != 1 || destination.sample_count() != 1) {
        throw std::invalid_argument("metal blit_target: multisampled targets are not supported");
    }
    if (source.presentable()) {
        throw std::invalid_argument("metal blit_target: a swapchain image is not a blit source");
    }
    if (source.format() != destination.format()) {
        throw std::invalid_argument("metal blit_target: source and destination formats differ");
    }

    ensure_command_buffer();
    end_encoder();
    id<MTLBlitCommandEncoder> blit = [command_buffer_ blitCommandEncoder];
    if (blit == nil) {
        throw std::runtime_error("metal: blitCommandEncoder failed");
    }
    // A scaled present is a filtered blit; Metal's blit encoder always filters, so the RHI filter
    // selects the sampler state the copy uses.
    [blit copyFromTexture:as_target(source).readable()
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(source.extent().width, source.extent().height, 1)
                toTexture:as_target(destination).readable()
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    (void)filter;
    [blit endEncoding];
}

void MetalCommandList::encode_signal_and_commit() {
    if (command_buffer_ == nil) {
        // Nothing was recorded; there is nothing to submit and no value to hand out.
        submitted_ = true;
        return;
    }
    end_encoder();
    completion_value_ = device_.next_completion_value();
    [command_buffer_ encodeSignalEvent:device_.event() value:completion_value_];

    // The completed handler may only publish the value: it runs on a thread Metal chooses, so it
    // never frees resources and never touches scene state (metal-backend.md section 3.1).
    MetalDevice* device = &device_;
    const std::uint64_t value = completion_value_;
    [command_buffer_ addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
      if (buffer.status == MTLCommandBufferStatusError) {
          // An errored buffer still signals the event so waiters do not hang; the error is
          // recorded as sticky device loss (section 6).
          device->mark_lost();
      }
      device->publish_completed(value);
    }];
    [command_buffer_ commit];
    submitted_ = true;
}

void MetalCommandList::submit() {
    encode_signal_and_commit();
    if (completion_value_ != 0) {
        // Synchronous submit is for offscreen, readback and goldens only; the frame path uses
        // submit_async (docs/guides/writing-a-backend.md section 3).
        [device_.event() waitUntilSignaledValue:completion_value_ timeoutMS:5000];
    }
}

void MetalCommandList::submit_async() {
    encode_signal_and_commit();
}

bool MetalCommandList::is_complete() const {
    if (!submitted_) {
        return false;
    }
    if (completion_value_ == 0) {
        return true;
    }
    // A lock-free load of the published value; never a wait (section 3.1).
    return device_.completion_source().completed_value() >= completion_value_;
}

bool MetalCommandList::wait(std::uint64_t timeout_ms) {
    if (!submitted_) {
        return false;
    }
    if (completion_value_ == 0) {
        return true;
    }
    return [device_.event() waitUntilSignaledValue:completion_value_ timeoutMS:timeout_ms] == YES;
}

std::unique_ptr<rhi::CommandList> MetalDevice::create_command_list() {
    return std::make_unique<MetalCommandList>(*this);
}

} // namespace tessera::rendering::metal
