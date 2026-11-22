#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_local;
layout(location = 3) in vec2 v_half_size;
layout(location = 4) in float v_radius;
layout(location = 5) flat in float v_mode;
layout(location = 6) in vec4 v_carve;
layout(location = 7) flat in uint v_texture_index;

// TESSERA_SHADER_PORTABLE selects the cross-compilation interface (ADR-049): the push-constant
// block becomes a std140 uniform buffer at binding 0 and the combined image sampler splits into a
// texture at binding 1 and a sampler at binding 2, the layout WebGPU, Metal and D3D12 use. The
// Vulkan build never defines it, so its SPIR-V is unchanged.
//
// TESSERA_TEXTURE_ARRAY selects the descriptor-indexing variant (RHI 1.7, issue #480 part 2): the
// single combined image sampler becomes an array of them, indexed by the per-vertex
// `v_texture_index`. It is a separate compilation of this same source, so the two variants cannot
// drift in their clip/SDF/shadow logic - only the sampling line differs. `nonuniformEXT` is
// required because the index is per-vertex data, not a uniform.
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 1) uniform texture2D u_texture;
layout(set = 0, binding = 2) uniform sampler u_sampler;
#define TESSERA_SAMPLE(uv) texture(sampler2D(u_texture, u_sampler), (uv))
// Integer reads go through a combined sampler too: `textureSize`/`texelFetch` on the bare texture
// would need GL_EXT_samplerless_texture_functions.
#define TESSERA_TEXTURE_SIZE textureSize(sampler2D(u_texture, u_sampler), 0)
#define TESSERA_FETCH(p) texelFetch(sampler2D(u_texture, u_sampler), (p), 0)
#elif defined(TESSERA_TEXTURE_ARRAY)
// The index is per-vertex data, so the array access is non-uniform and needs the extension.
#extension GL_EXT_nonuniform_qualifier : require
layout(set = 0, binding = 0) uniform sampler2D u_textures[32];
#define TESSERA_SAMPLE(uv) texture(u_textures[nonuniformEXT(v_texture_index)], (uv))
#define TESSERA_TEXTURE_SIZE textureSize(u_textures[nonuniformEXT(v_texture_index)], 0)
#define TESSERA_FETCH(p) texelFetch(u_textures[nonuniformEXT(v_texture_index)], (p), 0)
#else
layout(set = 0, binding = 0) uniform sampler2D u_texture;
#define TESSERA_SAMPLE(uv) texture(u_texture, (uv))
#define TESSERA_TEXTURE_SIZE textureSize(u_texture, 0)
#define TESSERA_FETCH(p) texelFetch(u_texture, (p), 0)
#endif

// The clip-record buffer (ADR-068 slice 3): set 1, a read-only storage buffer holding the frame's
// clip records and the interned set ranges. Set 0 is the sampled image, so a draw samples a texture
// and reads its clips in the same pass.
//
// Layout must match gpu::clip::ClipRecord / ClipSetRange / ClipBufferHeader (clip_records.hpp):
//   Header   { uint record_count; uint reserved[3]; }
//   Record   { uint kind; uint flags; uint first_vertex; uint vertex_count;
//              vec4 rect; vec4 radii_a; vec4 radii_b; }        // 64 bytes
//   SetRange { uint first_record; uint count; uint reserved[2]; }
//
// The sets live at a fixed offset after the record area, so the shader's array base is a constant.
#define TESSERA_CLIP_MAX_RECORDS 256
#define TESSERA_CLIP_MAX_SETS 256

struct ClipRecord {
    uint kind;
    uint flags;
    uint first_vertex;
    uint vertex_count;
    vec4 rect;    // xy = origin, zw = size in target pixels
    vec4 radii_a; // top-left, top-right, bottom-right, bottom-left
    vec4 radii_b; // elliptical second components; zero for a circular corner
};

struct ClipSetRange {
    uint first_record;
    uint count;
    uint reserved0;
    uint reserved1;
};

layout(set = 1, binding = 0, std430) readonly buffer ClipBuffer {
    uint record_count;
    uint header_reserved[3];
    ClipRecord records[TESSERA_CLIP_MAX_RECORDS];
    ClipSetRange sets[TESSERA_CLIP_MAX_SETS];
} clip_buffer;

// Must match Batched2DPushConstants (16 bytes: 2 floats + 1 float + 1 uint).
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 0, std140) uniform PushConstants {
#else
layout(push_constant) uniform PushConstants {
#endif
    vec2 viewport;
    float aa_radius;
    uint clip_set;
} pc;

layout(location = 0) out vec4 out_color;

float sd_round_box(vec2 p, vec2 b, float r) {
    vec2 q = abs(p) - b + vec2(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - r;
}

// erf, Abramowitz and Stegun 7.1.26 (maximum error about 1.5e-7): the Gaussian edge of a soft
// shadow is 0.5 * (1 - erf(d / (sigma * sqrt(2)))).
float erf_approx(float x) {
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    float y = 1.0 - poly * exp(-a * a);
    return x < 0.0 ? -y : y;
}

// Hard-edged per-corner rounded-rect clip test, used when AA is off (golden mode, aa_radius 0): the
// pixel-center rule keeps it deterministic and never writes outside the clip.
// The record arrives by index, not by value: recent glslang (16.x, the Linux distro build) lowers a
// by-value struct parameter to a duplicated struct type plus OpCopyLogical, which naga's SPIR-V
// front end rejects, so the cross-compiled WGSL (TESSERA_SHADER_CROSS) could not be produced on
// those hosts. Indexing the buffer is the same code with no per-call copy and translates everywhere.
bool inside_clip(vec2 p, uint index) {
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 size = clip_buffer.records[index].rect.zw;
    vec4 radii = clip_buffer.records[index].radii_a;
    if (p.x < origin.x || p.y < origin.y || p.x > origin.x + size.x ||
        p.y > origin.y + size.y) {
        return false;
    }
    vec2 q = p - origin;
    if (q.x < radii.x && q.y < radii.x) {
        vec2 d = vec2(q.x - radii.x, q.y - radii.x);
        return dot(d, d) <= radii.x * radii.x;
    }
    if (q.x > size.x - radii.y && q.y < radii.y) {
        vec2 d = vec2(q.x - (size.x - radii.y), q.y - radii.y);
        return dot(d, d) <= radii.y * radii.y;
    }
    if (q.x > size.x - radii.z && q.y > size.y - radii.z) {
        vec2 d = vec2(q.x - (size.x - radii.z), q.y - (size.y - radii.z));
        return dot(d, d) <= radii.z * radii.z;
    }
    if (q.x < radii.w && q.y > size.y - radii.w) {
        vec2 d = vec2(q.x - radii.w, q.y - (size.y - radii.w));
        return dot(d, d) <= radii.w * radii.w;
    }
    return true;
}

// Anti-aliased coverage of one rounded clip: the clip's own signed distance (framebuffer pixels,
// per-corner radii) blended over +/- aa_radius pixels, the same half-pixel width as the SDF edges.
// A pixel-aligned clip edge is fully covered on its inside pixels, so aligned clips look unchanged.
float clip_coverage(vec2 p, uint index) {
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 half_size = clip_buffer.records[index].rect.zw * 0.5;
    vec4 radii = clip_buffer.records[index].radii_a;
    vec2 q = p - (origin + half_size);
    float r = q.x < 0.0 ? (q.y < 0.0 ? radii.x : radii.w) : (q.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(half_size.x, half_size.y));
    float d = sd_round_box(q, half_size, r);
    return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
}

void main() {
    // Local units per device pixel, measured from the interpolated local coordinate (linear, so
    // the derivative is exact). It is 1.0 for an untransformed quad and 1/scale under a scale or
    // rotation, which keeps the SDF edge about aa_radius device pixels wide instead of aa_radius
    // local units (antialiasing.md section 5). Taken before any branch so it stays in uniform
    // control flow for the portable (WGSL) variant.
    vec2 local_dx = dFdx(v_local);
    vec2 local_dy = dFdy(v_local);
    float local_per_pixel = max(max(length(vec2(local_dx.x, local_dy.x)),
                                    length(vec2(local_dx.y, local_dy.y))), 1e-6);
    float edge_radius = pc.aa_radius * local_per_pixel;

    // Rounded clips are applied as a coverage mask: outside any clip alpha becomes 0 and the
    // alpha-blend leaves the destination untouched. This deliberately avoids `discard`, so the
    // shader does not require shaderDemoteToHelperInvocation (same policy as the SDF AA path).
    // With AA on the mask has anti-aliased coverage; with aa_radius 0 it is the hard test.
    //
    // The set is a range into the record list (ADR-068 slice 3), so the loop bound is data, not a
    // constant: an unclipped draw has count 0 and the loop body never runs, which is the early-out
    // that keeps an unclipped frame at its old cost.
    float clip_alpha = 1.0;
    ClipSetRange clip_set = clip_buffer.sets[pc.clip_set];
    for (uint offset = 0u; offset < clip_set.count; ++offset) {
        uint index = clip_set.first_record + offset;
        if (pc.aa_radius <= 0.0) {
            if (!inside_clip(gl_FragCoord.xy, index)) {
                clip_alpha = 0.0;
            }
        } else {
            clip_alpha *= clip_coverage(gl_FragCoord.xy, index);
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

    if (v_mode > 4000.0) {
        // Exact nearest texture (#682, kExactNearestMode): the texel comes from integer math on the
        // fragment's device pixel, so every GPU agrees with the CPU rule
        // `min(floor((2p + 1) * src / (2 * dst)), src - 1)`. The hardware nearest sampler is not
        // exact when a sample lands on a texel boundary (a 2:1 downscale picks the wrong neighbor on
        // every pixel). `v_carve` carries the integer device rectangle: xy origin, zw size. It is
        // rounded because interpolating a constant can leave a ulp of error. A texel fetch has no
        // derivative, so this branch is legal in non-uniform control flow for the portable variant.
        uvec2 src_size = uvec2(TESSERA_TEXTURE_SIZE);
        vec4 box = round(v_carve);
        uvec2 dst_size = uvec2(max(box.zw, vec2(1.0)));
        ivec2 offset = ivec2(floor(gl_FragCoord.xy)) - ivec2(box.xy);
        uvec2 p = uvec2(clamp(offset, ivec2(0), ivec2(dst_size) - ivec2(1)));
        uvec2 s = min(((2u * p + uvec2(1u)) * src_size) / (2u * dst_size), src_size - uvec2(1u));
        color *= TESSERA_FETCH(ivec2(s));
    } else if (v_mode > 0.5 && v_mode < 1.5) {
        float d = sd_round_box(v_local, v_half_size, v_radius);
        float alpha;
        if (pc.aa_radius <= 0.0) {
            alpha = d <= 0.0 ? 1.0 : 0.0;
        } else {
            alpha = 1.0 - smoothstep(-edge_radius, edge_radius, d);
        }
        color.a *= alpha;
    } else if (v_mode > 1.5 && v_mode < 2.5) {
        vec4 texel_color = TESSERA_TEXEL;
        texel_color.a = min(texel_color.a * (1.0 + (v_mode - 2.0) * 4.0), 1.0);
        color *= texel_color;
    } else if (v_mode > 1000.0) {
        // Box shadow: mode is 1024 + sigma and the quad is the box grown by 2 sigma. sigma 0 is the
        // hard-edged shadow (`box-shadow: 4px 4px 0`): the same shape with a coverage step, so the
        // sharp and blurred cases share one quad and one carve test.
        float sigma = v_mode - 1024.0;
        vec2 shadow_half = v_half_size - vec2(2.0 * sigma);
        float d = sd_round_box(v_local, shadow_half, v_radius);
        float cover;
        if (sigma > 0.0) {
            cover = 0.5 * (1.0 - erf_approx(d / (sigma * 1.41421356)));
        } else if (pc.aa_radius <= 0.0) {
            cover = d <= 0.0 ? 1.0 : 0.0;
        } else {
            cover = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        // CSS paints an outer shadow only outside the border box (CSS Backgrounds 3 section 7.1.1):
        // `v_carve` is that box, so the coverage is 0 inside it. Its corner radius is the shadow's
        // radius minus the uniform inset (an engine `border-radius` is one length), which also keeps
        // the cut inside a clamped radius.
        if (v_carve.z > 0.0 && v_carve.w > 0.0) {
            float inset = v_half_size.x - 2.0 * sigma - v_carve.z;
            vec2 hole_half = v_carve.zw;
            float hole_radius = clamp(v_radius - inset, 0.0, min(hole_half.x, hole_half.y));
            if (sd_round_box(v_local - v_carve.xy, hole_half, hole_radius) <= 0.0) {
                cover = 0.0;
            }
        }
        color.a *= cover;
    } else if (v_mode > 2000.0) {
        // Inset box shadow (issue #612): mode is 2048 + sigma. The quad is the element's border box
        // and `v_carve` is the inner box (the border box moved by the offset and shrunk by the
        // spread), so the shadow fills the ring between them and fades inward. The Gaussian is
        // evaluated on the inner box's signed distance, which is the same edge profile an outer
        // shadow has, mirrored: coverage is 1 at the inner box and falls off outward from it.
        float sigma = v_mode - 2048.0;
        vec2 inner_half = v_carve.z > 0.0 && v_carve.w > 0.0 ? v_carve.zw : v_half_size;
        vec2 inner_centre = v_carve.z > 0.0 && v_carve.w > 0.0 ? v_carve.xy : vec2(0.0);
        // The inner radius is the border radius minus the spread, which is the inset between the
        // two boxes on the x axis (an engine `border-radius` is one length).
        float inset = v_half_size.x - inner_half.x;
        float inner_radius = clamp(v_radius - inset, 0.0, min(inner_half.x, inner_half.y));
        float d = sd_round_box(v_local - inner_centre, inner_half, inner_radius);
        float cover;
        if (sigma > 0.0) {
            // Inside the inner box (d < 0) the shadow is at full strength; outside it the Gaussian
            // tail fades toward the border box.
            cover = 0.5 * (1.0 + erf_approx(-d / (sigma * 1.41421356)));
        } else if (pc.aa_radius <= 0.0) {
            cover = d <= 0.0 ? 1.0 : 0.0;
        } else {
            cover = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        // The quad is the border box, so the rounded silhouette clips the shadow: a pixel outside
        // the border box's rounded shape paints nothing.
        float outer = sd_round_box(v_local, v_half_size, v_radius);
        if (pc.aa_radius <= 0.0) {
            cover = outer <= 0.0 ? cover : 0.0;
        } else {
            cover *= 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, outer);
        }
        color.a *= cover;
    } else if (v_mode > 2.5) {
        // Rounded-rect outline: mode is 3 + the stroke width. The ring is the outer box minus the
        // same box inset by the width, so its distance is max(outer, -inner).
        float w = v_mode - 3.0;
        float outer = sd_round_box(v_local, v_half_size, v_radius);
        float inner = sd_round_box(v_local, v_half_size - vec2(w), max(v_radius - w, 0.0));
        float d = max(outer, -inner);
        float alpha;
        if (pc.aa_radius <= 0.0) {
            alpha = d <= 0.0 ? 1.0 : 0.0;
        } else {
            alpha = 1.0 - smoothstep(-edge_radius, edge_radius, d);
        }
        color.a *= alpha;
    }

    color.a *= clip_alpha;
    out_color = color;
}
