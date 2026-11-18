// SPDX-License-Identifier: MIT
//
// Mip chain downsample (RHI 1.6, issue #510). WebGPU has no blit, so a chain is built one level at
// a time: a fullscreen triangle samples level N-1 with a linear sampler and writes level N. The
// 2:1 bilinear tap is the same box-style average the Vulkan backend gets from vkCmdBlitImage with
// VK_FILTER_LINEAR. The texture format decides the encoding: an sRGB view decodes on sample and
// encodes on write, so the average is taken in linear light exactly as the sampler would.
//
// Hand-authored (ADR-011); there is no GLSL twin, so it is validated by naga and the Wasm lane only.

@group(0) @binding(0) var u_source: texture_2d<f32>;
@group(0) @binding(1) var u_sampler: sampler;

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
};

// One oversized triangle covers the target: vertices (-1, 1), (3, 1), (-1, -3) with uv (0, 0),
// (2, 0), (0, 2), so uv (0, 0) is the top-left texel exactly as the texture is stored.
@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> VertexOutput {
    let u = f32((index << 1u) & 2u);
    let v = f32(index & 2u);
    var out: VertexOutput;
    out.position = vec4<f32>(u * 2.0 - 1.0, 1.0 - v * 2.0, 0.0, 1.0);
    out.uv = vec2<f32>(u, v);
    return out;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    return textureSample(u_source, u_sampler, in.uv);
}
