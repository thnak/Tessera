#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_local;
layout(location = 3) in vec2 v_half_size;
layout(location = 4) in float v_radius;
layout(location = 5) flat in float v_mode;

// TESSERA_SHADER_PORTABLE selects the cross-compilation interface (ADR-049): the push-constant
// block becomes a std140 uniform buffer at binding 0 and the combined image sampler splits into a
// texture at binding 1 and a sampler at binding 2, the layout WebGPU, Metal and D3D12 use. The
// Vulkan build never defines it, so its SPIR-V is unchanged.
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 1) uniform texture2D u_texture;
layout(set = 0, binding = 2) uniform sampler u_sampler;
#define TESSERA_SAMPLE(uv) texture(sampler2D(u_texture, u_sampler), (uv))
#else
layout(set = 0, binding = 0) uniform sampler2D u_texture;
#define TESSERA_SAMPLE(uv) texture(u_texture, (uv))
#endif

struct ClipUniform {
    vec4 rect;  // xy = origin, zw = size in target pixels
    vec4 radii; // top-left, top-right, bottom-right, bottom-left
};

// Must match Batched2DPushConstants (112 bytes: 2 floats + 1 float + 1 uint + 3 * 32-byte clips).
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

layout(location = 0) out vec4 out_color;

float sd_round_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// Hard-edged per-corner rounded-rect clip test (no clip AA in this slice; see antialiasing.md).
bool inside_clip(vec2 p, ClipUniform clip) {
    vec2 origin = clip.rect.xy;
    vec2 size = clip.rect.zw;
    if (p.x < origin.x || p.y < origin.y || p.x > origin.x + size.x ||
        p.y > origin.y + size.y) {
        return false;
    }
    vec2 q = p - origin;
    if (q.x < clip.radii.x && q.y < clip.radii.x) {
        vec2 d = vec2(q.x - clip.radii.x, q.y - clip.radii.x);
        return dot(d, d) <= clip.radii.x * clip.radii.x;
    }
    if (q.x > size.x - clip.radii.y && q.y < clip.radii.y) {
        vec2 d = vec2(q.x - (size.x - clip.radii.y), q.y - clip.radii.y);
        return dot(d, d) <= clip.radii.y * clip.radii.y;
    }
    if (q.x > size.x - clip.radii.z && q.y > size.y - clip.radii.z) {
        vec2 d = vec2(q.x - (size.x - clip.radii.z), q.y - (size.y - clip.radii.z));
        return dot(d, d) <= clip.radii.z * clip.radii.z;
    }
    if (q.x < clip.radii.w && q.y > size.y - clip.radii.w) {
        vec2 d = vec2(q.x - clip.radii.w, q.y - (size.y - clip.radii.w));
        return dot(d, d) <= clip.radii.w * clip.radii.w;
    }
    return true;
}

void main() {
    // Rounded clips are applied as a coverage mask: outside any clip alpha becomes 0 and the
    // alpha-blend leaves the destination untouched. This deliberately avoids `discard`, so the
    // shader does not require shaderDemoteToHelperInvocation (same policy as the SDF AA path).
    float clip_alpha = 1.0;
    if (pc.clip_count > 0u) {
        for (uint index = 0u; index < pc.clip_count; ++index) {
            if (!inside_clip(gl_FragCoord.xy, pc.clips[index])) {
                clip_alpha = 0.0;
            }
        }
    }

    vec4 color = v_color;
#ifdef TESSERA_SHADER_PORTABLE
    // WGSL allows implicit-derivative sampling only in uniform control flow, so the portable
    // variant samples before branching on the per-primitive mode, as batched2d.wgsl does.
    const vec4 texel = TESSERA_SAMPLE(v_uv);
#define TESSERA_TEXEL texel
#else
#define TESSERA_TEXEL TESSERA_SAMPLE(v_uv)
#endif

    if (v_mode > 0.5 && v_mode < 1.5) {
        float d = sd_round_box(v_local, v_half_size, v_radius);
        float alpha;
        if (pc.aa_radius <= 0.0) {
            alpha = d <= 0.0 ? 1.0 : 0.0;
        } else {
            alpha = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        color.a *= alpha;
    } else if (v_mode > 1.5) {
        color *= TESSERA_TEXEL;
    }

    color.a *= clip_alpha;
    out_color = color;
}
