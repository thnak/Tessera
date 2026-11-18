#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in vec2 in_local;
layout(location = 4) in vec2 in_half_size;
layout(location = 5) in float in_radius;
layout(location = 6) in float in_mode;
layout(location = 7) in vec4 in_carve;
// Index into the sampled-image array (RHI 1.7, issue #480 part 2). Always present in the vertex
// layout so one pipeline serves both paths: the one-set-per-texture path writes 0 and the shader
// reads the single bound image, the array path writes the batch's slot.
layout(location = 8) in uint in_texture_index;

struct ClipUniform {
    vec4 rect;  // xy = origin, zw = size in target pixels
    vec4 radii; // top-left, top-right, bottom-right, bottom-left
};

// Must match Batched2DPushConstants (112 bytes: 2 floats + 1 float + 1 uint + 3 * 32-byte clips).
// TESSERA_SHADER_PORTABLE selects the cross-compilation interface (ADR-049): the same block as a
// std140 uniform buffer at set 0, binding 0, the layout WebGPU, Metal and D3D12 use. The Vulkan
// build never defines it, so its SPIR-V is unchanged.
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 0, std140) uniform PushConstants {
#else
layout(push_constant) uniform PushConstants {
#endif
    vec2 viewport;
    float aa_radius;
    uint clip_count;
    ClipUniform clips[3];
} pc;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_color;
layout(location = 2) out vec2 v_local;
layout(location = 3) out vec2 v_half_size;
layout(location = 4) out float v_radius;
layout(location = 5) flat out float v_mode;
layout(location = 6) out vec4 v_carve;
// Flat: every vertex of a quad carries the same index, so interpolation would only add noise.
// The LCD variant (batched2d_lcd.frag) shares this stage but never samples an array, so it compiles
// with TESSERA_NO_TEXTURE_ARRAY and the output is absent there: a vertex output the fragment stage
// does not declare is legal, but the validation layer reports it and the LCD test treats that as a
// failure.
#ifndef TESSERA_NO_TEXTURE_ARRAY
layout(location = 7) flat out uint v_texture_index;
#endif

void main() {
    vec2 ndc = vec2(in_position.x / pc.viewport.x * 2.0 - 1.0,
                    in_position.y / pc.viewport.y * 2.0 - 1.0);
    gl_Position = vec4(ndc, 0.0, 1.0);

    v_uv = in_uv;
    v_color = in_color;
    v_local = in_local;
    v_half_size = in_half_size;
    v_radius = in_radius;
    v_mode = in_mode;
    v_carve = in_carve;
#ifndef TESSERA_NO_TEXTURE_ARRAY
    v_texture_index = in_texture_index;
#endif
}
