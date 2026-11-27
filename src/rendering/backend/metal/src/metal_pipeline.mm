// SPDX-License-Identifier: MIT

// Metal pipelines (ADR-053 slice 2, docs/design/gpu/metal-backend.md section 2).
//
// A pipeline is built from MSL, not SPIR-V: `PipelineDesc::shader_id` resolves against the MSL
// table (metal_shader_registry.hpp), which is generated at build time by ADR-049's cross-compile
// path. A pipeline that names an id with no MSL fails loudly rather than drawing nothing, and a
// pipeline that supplies SPIR-V but no id is rejected the same way: Metal cannot consume SPIR-V,
// and silently ignoring the words would produce an empty pipeline.
//
// The MSL is compiled with `newLibraryWithSource:` at pipeline creation. That is not "runtime
// shader compilation in production" in the sense the rule forbids (no shader *authoring* at run
// time, no shipping GLSL): the source is a build-time artifact embedded in the binary, and the
// compile happens once per pipeline, not per frame. A metallib precompile is the follow-up that
// removes even that cost; it needs the Metal toolchain, which the build host may not have.

#include "metal_internal.hpp"

#include "tessera/rendering/metal/metal_shader_registry.hpp"

#include <stdexcept>
#include <string>

namespace tessera::rendering::metal {

MetalPipeline::MetalPipeline(id<MTLRenderPipelineState> state, std::uint32_t sample_count,
                             std::uint32_t push_constant_size,
                             std::uint32_t fragment_push_constant_index,
                             std::uint32_t fragment_storage_buffer_index, bool uses_vertex_stream)
    : state_(state),
      sample_count_(sample_count),
      push_constant_size_(push_constant_size),
      fragment_push_constant_index_(fragment_push_constant_index),
      fragment_storage_buffer_index_(fragment_storage_buffer_index),
      uses_vertex_stream_(uses_vertex_stream) {}

std::uint32_t MetalPipeline::sample_count() const {
    return sample_count_;
}

namespace {

// Compiles one MSL source into a library. `what` names the stage in the error, so a shader that
// fails to compile says which one.
[[nodiscard]] id<MTLLibrary> compile_library(id<MTLDevice> device, std::string_view source,
                                             const char* what) {
    NSError* error = nil;
    NSString* text = [[NSString alloc] initWithBytes:source.data()
                                              length:source.size()
                                            encoding:NSUTF8StringEncoding];
    if (text == nil) {
        throw std::runtime_error(std::string("metal: ") + what + " MSL is not valid UTF-8");
    }
    id<MTLLibrary> library = [device newLibraryWithSource:text options:nil error:&error];
    if (library == nil) {
        std::string message = std::string("metal: ") + what + " MSL failed to compile";
        if (error != nil && error.localizedDescription != nil) {
            message += ": ";
            message += error.localizedDescription.UTF8String;
        }
        throw std::runtime_error(message);
    }
    return library;
}

[[nodiscard]] MTLVertexFormat to_metal_vertex_format(rhi::VertexFormat format) {
    return static_cast<MTLVertexFormat>(vertex_format(format));
}

} // namespace

std::unique_ptr<rhi::Pipeline> MetalDevice::create_pipeline(const rhi::PipelineDesc& desc) {
    if (desc.vertex_layout.stride == 0 && desc.vertex_layout.attribute_count > 0) {
        throw std::invalid_argument(
            "metal create_pipeline: a vertex attribute layout needs a non-zero stride");
    }
    if (!rhi::is_supported_sample_count(desc.sample_count)) {
        throw std::invalid_argument("metal create_pipeline: unsupported sample count");
    }
    // Dual-source blending needs the second fragment output. The device reports whether it can
    // express it, and a request it cannot honour is refused rather than mapped to another blend
    // (RHI 1.5, docs/design/gpu/rhi.md section 4.1).
    if (needs_dual_source(desc.blend) && !supports_dual_source_blend()) {
        throw std::runtime_error(
            "metal create_pipeline: dual-source blending is not supported on this device");
    }

    // Metal consumes MSL, so the shader id is the only way to name a program. SPIR-V words are
    // accepted by the RHI but cannot be used here; a caller that supplies only SPIR-V gets a clear
    // error instead of an empty pipeline.
    if (desc.shader_id.empty()) {
        throw std::invalid_argument(
            "metal create_pipeline: a shader_id is required (Metal consumes MSL, not SPIR-V)");
    }
    const MslProgram* program = find_msl_program(desc.shader_id);
    if (program == nullptr) {
        throw std::runtime_error("metal create_pipeline: no MSL program for shader id '" +
                                 std::string(desc.shader_id) +
                                 "' (configure with -DTESSERA_SHADER_CROSS=ON)");
    }
    if (program->vertex_source.empty() || program->fragment_source.empty()) {
        throw std::runtime_error("metal create_pipeline: shader id '" +
                                 std::string(desc.shader_id) + "' has an empty MSL stage");
    }
    // A read-only storage buffer (RHI 1.12, ADR-068) is only bindable if the program's fragment MSL
    // declares one; refuse the request instead of building a pipeline whose bind can never reach
    // the shader. The slot the pipeline reports is the program's, so a pipeline that did not ask
    // for the buffer reports none and bind_storage_buffer rejects it.
    if (desc.storage_buffer && !supports_storage_buffers()) {
        throw std::runtime_error("metal create_pipeline: this device has no storage buffers");
    }
    if (desc.storage_buffer && program->fragment_storage_buffer_index == kNoMslBuffer) {
        throw std::runtime_error("metal create_pipeline: shader id '" +
                                 std::string(desc.shader_id) +
                                 "' declares no storage buffer in its fragment MSL");
    }
    const std::uint32_t storage_index =
        desc.storage_buffer ? program->fragment_storage_buffer_index : kNoMslBuffer;

    id<MTLLibrary> vertex_library = compile_library(device_, program->vertex_source, "vertex");
    id<MTLLibrary> fragment_library = compile_library(device_, program->fragment_source, "fragment");

    NSString* vertex_name = [[NSString alloc] initWithUTF8String:program->vertex_entry.data()];
    NSString* fragment_name = [[NSString alloc] initWithUTF8String:program->fragment_entry.data()];
    id<MTLFunction> vertex_function = [vertex_library newFunctionWithName:vertex_name];
    id<MTLFunction> fragment_function = [fragment_library newFunctionWithName:fragment_name];
    if (vertex_function == nil || fragment_function == nil) {
        throw std::runtime_error("metal create_pipeline: MSL entry point not found");
    }

    MTLRenderPipelineDescriptor* descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex_function;
    descriptor.fragmentFunction = fragment_function;
    descriptor.rasterSampleCount = desc.sample_count;

    MTLRenderPipelineColorAttachmentDescriptor* attachment = descriptor.colorAttachments[0];
    attachment.pixelFormat = static_cast<MTLPixelFormat>(pixel_format(desc.color_format));
    const BlendState blend = blend_state(desc.blend);
    attachment.blendingEnabled = blend.enabled ? YES : NO;
    if (blend.enabled) {
        attachment.sourceRGBBlendFactor = static_cast<MTLBlendFactor>(blend.source_color);
        attachment.destinationRGBBlendFactor = static_cast<MTLBlendFactor>(blend.destination_color);
        attachment.rgbBlendOperation = static_cast<MTLBlendOperation>(blend.color_operation);
        attachment.sourceAlphaBlendFactor = static_cast<MTLBlendFactor>(blend.source_alpha);
        attachment.destinationAlphaBlendFactor =
            static_cast<MTLBlendFactor>(blend.destination_alpha);
        attachment.alphaBlendOperation = static_cast<MTLBlendOperation>(blend.alpha_operation);
    }

    // The vertex layout comes from the RHI descriptor, not from the shader: the MSL declares
    // `[[stage_in]]` attributes by location, and Metal needs the stride and formats here.
    //
    // The stream sits at kVertexStreamIndex, not buffer(0): the vertex stage's push-constant block
    // is buffer(0), and a vertex stream there would be overwritten by it. A fullscreen program has
    // no attributes (it reads `vertex_id`), so it gets no descriptor at all.
    if (desc.vertex_layout.attribute_count > 0) {
        MTLVertexDescriptor* vertex_descriptor = [[MTLVertexDescriptor alloc] init];
        vertex_descriptor.layouts[kVertexStreamIndex].stride = desc.vertex_layout.stride;
        vertex_descriptor.layouts[kVertexStreamIndex].stepFunction = MTLVertexStepFunctionPerVertex;
        for (std::uint32_t index = 0; index < desc.vertex_layout.attribute_count; ++index) {
            const rhi::VertexAttribute& attribute = desc.vertex_layout.attributes[index];
            vertex_descriptor.attributes[attribute.location].format =
                to_metal_vertex_format(attribute.format);
            vertex_descriptor.attributes[attribute.location].offset = attribute.offset;
            vertex_descriptor.attributes[attribute.location].bufferIndex = kVertexStreamIndex;
        }
        descriptor.vertexDescriptor = vertex_descriptor;
    }

    NSError* error = nil;
    id<MTLRenderPipelineState> state = [device_ newRenderPipelineStateWithDescriptor:descriptor
                                                                              error:&error];
    if (state == nil) {
        std::string message = "metal create_pipeline: newRenderPipelineStateWithDescriptor failed";
        if (error != nil && error.localizedDescription != nil) {
            message += ": ";
            message += error.localizedDescription.UTF8String;
        }
        throw std::runtime_error(message);
    }

    return std::make_unique<MetalPipeline>(state, desc.sample_count, desc.push_constant_size,
                                           program->fragment_push_constant_index, storage_index,
                                           desc.vertex_layout.attribute_count > 0);
}

} // namespace tessera::rendering::metal
