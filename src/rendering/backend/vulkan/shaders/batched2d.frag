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
//   Header   { uint record_count; uint vertex_count; uint reserved[2]; }
//   Record   { uint kind; uint flags; uint first_vertex; uint vertex_count;
//              vec4 rect; vec4 radii_a; vec4 radii_b; }        // 64 bytes
//   SetRange { uint first_record; uint count; uint reserved[2]; }
//   Vertex   { vec2 position; }                                // polygon pool (slice 5), after the sets
//
// The sets live at a fixed offset after the record area, so the shader's array base is a constant.
#define TESSERA_CLIP_MAX_RECORDS 256
#define TESSERA_CLIP_MAX_SETS 256
#define TESSERA_CLIP_MAX_VERTICES 4096

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

// The gradient area (ADR-076) follows the clip vertex pool, so no offset above moved. A gradient quad
// is one quad: its vertices carry mode 8192 and the record index in carve.x, and the shader below
// evaluates the ramp at the pixel centre. Layout must match gpu::gradient::GradientRecord
// (gradient_records.hpp):
//   GradientRecord { uint kind; uint stop_count; uint first_stop; uint flags; vec4 params; } // 32 bytes
//   stop_colors[s] (vec4) and stop_offsets[s] (float), both indexed by first_stop.
#define TESSERA_GRADIENT_MAX_RECORDS 512
#define TESSERA_GRADIENT_MAX_STOPS 2048
#define TESSERA_GRADIENT_MAX_STOPS_PER_RECORD 32u

struct GradientRecord {
    uint kind;       // 0 linear, 1 radial, 2 conic
    uint stop_count;
    uint first_stop;
    uint flags;
    vec4 params;     // quad-local geometry, see gradient_records.hpp
};

layout(set = 1, binding = 0, std430) readonly buffer ClipBuffer {
    uint record_count;
    uint vertex_count;
    uint gradient_count;
    uint gradient_stop_count;
    ClipRecord records[TESSERA_CLIP_MAX_RECORDS];
    ClipSetRange sets[TESSERA_CLIP_MAX_SETS];
    vec2 vertices[TESSERA_CLIP_MAX_VERTICES]; // polygon vertex pool (ADR-068 slice 5)
    GradientRecord gradients[TESSERA_GRADIENT_MAX_RECORDS];
    vec4 stop_colors[TESSERA_GRADIENT_MAX_STOPS];
    float stop_offsets[TESSERA_GRADIENT_MAX_STOPS];
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

// Hard-edged clip test, used when AA is off (golden mode, aa_radius 0): the pixel-center rule keeps
// it deterministic and never writes outside the clip. It dispatches on the record's kind (ADR-068
// slice 4): an `Ellipse` record is a circle/ellipse, a `RoundedRect` is a rect with per-corner
// elliptical radii, and a `Polygon` is its bounding box until slice 5.
//
// The record arrives by index, not by value: recent glslang (16.x, the Linux distro build) lowers a
// by-value struct parameter to a duplicated struct type plus OpCopyLogical, which naga's SPIR-V
// front end rejects, so the cross-compiled WGSL (TESSERA_SHADER_CROSS) could not be produced on
// those hosts. Indexing the buffer is the same code with no per-call copy and translates everywhere.
//
// `radii_b` carries the vertical radius of each corner's elliptical pair; a zero component means the
// corner is circular and the horizontal radius is used for both, which is what a slice 3 record
// looks like.
bool inside_ellipse_record(vec2 p, uint index) {
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 size = clip_buffer.records[index].rect.zw;
    vec2 half_size = size * 0.5;
    if (half_size.x <= 0.0 || half_size.y <= 0.0) {
        return false;
    }
    vec2 d = (p - (origin + half_size)) / half_size;
    return dot(d, d) <= 1.0;
}

// Polygon clip (ADR-068 slice 5): the even-odd / non-zero ray test, expression for expression the same
// as gpu::clip::inside_polygon (clip_shapes.hpp), so the shader and the CPU reference agree on every
// pixel centre that is not within rounding of an edge. The vertices come from the frame's pool; the
// loop bound is the record's own count, which the renderer never lets past 256.
bool inside_polygon_record(vec2 p, uint index) {
    uint first = clip_buffer.records[index].first_vertex;
    uint count = min(clip_buffer.records[index].vertex_count, 256u);
    bool even_odd = (clip_buffer.records[index].flags & 1u) != 0u;
    bool parity = false;
    int winding = 0;
    for (uint i = 0u; i < count; ++i) {
        vec2 a = clip_buffer.vertices[first + i];
        vec2 b = clip_buffer.vertices[first + (i + 1u == count ? 0u : i + 1u)];
        if ((a.y <= p.y) == (b.y <= p.y)) {
            continue;
        }
        float t = (p.y - a.y) / (b.y - a.y);
        float crossing_x = a.x + t * (b.x - a.x);
        if (p.x < crossing_x) {
            parity = !parity;
            winding += b.y > a.y ? 1 : -1;
        }
    }
    return even_odd ? parity : winding != 0;
}

// Signed distance (pixels, negative inside) from p to the polygon's outline: the nearest segment
// gives the magnitude, the ray test the sign. Used only by the anti-aliased path.
float polygon_signed_distance(vec2 p, uint index) {
    uint first = clip_buffer.records[index].first_vertex;
    uint count = min(clip_buffer.records[index].vertex_count, 256u);
    float nearest = 1e30;
    for (uint i = 0u; i < count; ++i) {
        vec2 a = clip_buffer.vertices[first + i];
        vec2 b = clip_buffer.vertices[first + (i + 1u == count ? 0u : i + 1u)];
        vec2 edge = b - a;
        vec2 to_point = p - a;
        float length_squared = dot(edge, edge);
        float h = length_squared > 0.0 ? clamp(dot(to_point, edge) / length_squared, 0.0, 1.0) : 0.0;
        vec2 offset = to_point - edge * h;
        nearest = min(nearest, dot(offset, offset));
    }
    float distance = sqrt(nearest);
    return inside_polygon_record(p, index) ? -distance : distance;
}

bool inside_clip(vec2 p, uint index) {
    if (clip_buffer.records[index].kind == 2u) {
        return inside_polygon_record(p, index);
    }
    if (clip_buffer.records[index].kind == 1u) {
        return inside_ellipse_record(p, index);
    }
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 size = clip_buffer.records[index].rect.zw;
    vec4 radii = clip_buffer.records[index].radii_a;
    vec4 radii_v = clip_buffer.records[index].radii_b;
    if (p.x < origin.x || p.y < origin.y || p.x > origin.x + size.x ||
        p.y > origin.y + size.y) {
        return false;
    }
    vec2 q = p - origin;
    // A zero vertical component means a circular corner: use the horizontal radius for both axes.
    vec4 vertical = vec4(radii_v.x > 0.0 ? radii_v.x : radii.x,
                         radii_v.y > 0.0 ? radii_v.y : radii.y,
                         radii_v.z > 0.0 ? radii_v.z : radii.z,
                         radii_v.w > 0.0 ? radii_v.w : radii.w);
    if (radii.x > 0.0 && q.x < radii.x && q.y < vertical.x) {
        vec2 d = vec2((q.x - radii.x) / radii.x, (q.y - vertical.x) / vertical.x);
        return dot(d, d) <= 1.0;
    }
    if (radii.y > 0.0 && q.x > size.x - radii.y && q.y < vertical.y) {
        vec2 d = vec2((q.x - (size.x - radii.y)) / radii.y, (q.y - vertical.y) / vertical.y);
        return dot(d, d) <= 1.0;
    }
    if (radii.z > 0.0 && q.x > size.x - radii.z && q.y > size.y - vertical.z) {
        vec2 d = vec2((q.x - (size.x - radii.z)) / radii.z,
                      (q.y - (size.y - vertical.z)) / vertical.z);
        return dot(d, d) <= 1.0;
    }
    if (radii.w > 0.0 && q.x < radii.w && q.y > size.y - vertical.w) {
        vec2 d = vec2((q.x - radii.w) / radii.w, (q.y - (size.y - vertical.w)) / vertical.w);
        return dot(d, d) <= 1.0;
    }
    return true;
}

// Anti-aliased coverage of one clip: the clip's own signed distance (framebuffer pixels) blended over
// +/- aa_radius pixels, the same half-pixel width as the SDF edges. A pixel-aligned clip edge is
// fully covered on its inside pixels, so aligned clips look unchanged.
//
// An `Ellipse` record uses the ellipse's own distance approximation; a `RoundedRect` uses the
// rounded-box distance with the corner radius that applies at the pixel's quadrant. The elliptical
// pair is not used on this path: an elliptical corner's exact distance is not a rounded-box distance,
// and the AA path is a one-pixel edge refinement, so the horizontal radius is the documented
// approximation (the hard test, which is what goldens use, is exact).
float clip_coverage(vec2 p, uint index) {
    if (clip_buffer.records[index].kind == 2u) {
        return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, polygon_signed_distance(p, index));
    }
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 half_size = clip_buffer.records[index].rect.zw * 0.5;
    vec2 q = p - (origin + half_size);
    if (clip_buffer.records[index].kind == 1u) {
        // Normalised ellipse distance, scaled back to pixels by the smaller radius: exact on the
        // axes and a close approximation between them, which is all a one-pixel edge needs.
        if (half_size.x <= 0.0 || half_size.y <= 0.0) {
            return 0.0;
        }
        vec2 d = q / half_size;
        float scale = min(half_size.x, half_size.y);
        float distance = (length(d) - 1.0) * scale;
        return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, distance);
    }
    vec4 radii = clip_buffer.records[index].radii_a;
    float r = q.x < 0.0 ? (q.y < 0.0 ? radii.x : radii.w) : (q.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(half_size.x, half_size.y));
    float d = sd_round_box(q, half_size, r);
    return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
}

// The ramp position of a gradient record at a quad-local point (pixels from the quad's centre):
// gpu::gradient::position (gradient_math.hpp), expression for expression, so the CPU reference and
// this shader cannot drift. The record is read by index, field by field, for the same naga reason as
// the clip helpers above.
float gradient_position(uint record, vec2 local) {
    uint kind = clip_buffer.gradients[record].kind;
    vec4 params = clip_buffer.gradients[record].params;
    if (kind == 1u) {
        vec2 n = (local - params.xy) / params.zw;
        return sqrt(n.x * n.x + n.y * n.y);
    }
    if (kind == 2u) {
        vec2 d = local - params.xy;
        // CSS angles run clockwise from 12 o'clock; the exact centre has no angle and takes 0
        // (atan(0, 0) is undefined in GLSL).
        float angle = (d.x == 0.0 && d.y == 0.0) ? 0.0 : atan(d.x, -d.y);
        float turn = angle / 6.28318530717958647692 - params.z;
        return turn - floor(turn);
    }
    return local.x * params.x + local.y * params.y + params.z;
}

// Encoded-space interpolation over the record's sorted stops: gpu::gradient::ramp. Find the last stop
// at or below t, then blend it with the next one; a duplicate offset lets the later stop win, and t
// outside the ramp takes the end stop. The loop runs only for a gradient fragment, bounded by the
// record's own count (never more than 32).
vec4 gradient_color(uint record, float t) {
    uint count = min(clip_buffer.gradients[record].stop_count, TESSERA_GRADIENT_MAX_STOPS_PER_RECORD);
    uint first = clip_buffer.gradients[record].first_stop;
    if (count == 0u) {
        return vec4(0.0);
    }
    if (count == 1u) {
        return clip_buffer.stop_colors[first];
    }
    t = clamp(t, 0.0, 1.0);
    int below = -1;
    for (uint i = 0u; i < count; ++i) {
        if (clip_buffer.stop_offsets[first + i] <= t) {
            below = int(i);
        }
    }
    if (below < 0) {
        return clip_buffer.stop_colors[first];
    }
    uint k = uint(below);
    float offset_k = clip_buffer.stop_offsets[first + k];
    if (offset_k == t || k + 1u == count) {
        return clip_buffer.stop_colors[first + k];
    }
    vec4 a = clip_buffer.stop_colors[first + k];
    vec4 b = clip_buffer.stop_colors[first + k + 1u];
    float local_t = (t - offset_k) / (clip_buffer.stop_offsets[first + k + 1u] - offset_k);
    // Premultiplied, then un-premultiplied (the blend stage composites straight alpha): CSS Images
    // 3.5.3 and Chromium. gpu::gradient::ramp, expression for expression.
    float alpha = a.a + (b.a - a.a) * local_t;
    vec3 premultiplied = a.rgb * a.a + (b.rgb * b.a - a.rgb * a.a) * local_t;
    vec3 rgb = alpha > 0.0 ? premultiplied / alpha : a.rgb + (b.rgb - a.rgb) * local_t;
    return vec4(rgb, alpha);
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
    //
    // The range is read field by field rather than copied into a local. A by-value struct copy
    // lowers to OpCopyLogical, which naga's SPIR-V front end rejects, so the cross-compiled WGSL
    // (TESSERA_SHADER_CROSS) could not be produced - the same hazard the clip helpers below avoid.
    float clip_alpha = 1.0;
    uint first_record = clip_buffer.sets[pc.clip_set].first_record;
    uint clip_count = clip_buffer.sets[pc.clip_set].count;
    for (uint offset = 0u; offset < clip_count; ++offset) {
        uint index = first_record + offset;
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

    if (v_mode > 8000.0) {
        // Gradient quad (ADR-076, kGradientMode): the record index rides in carve.x, rounded because
        // interpolating a constant can leave a ulp of error. Evaluated at the pixel centre in the
        // quad's local space, so a gradient under a transform rotates with its box. The ramp is the
        // straight-alpha colour, like a solid quad's vertex colour, and v_color (white, carrying only
        // the flat-layer opacity) multiplies it; the rounded-clip coverage applies below as ever.
        uint record = min(uint(round(v_carve.x)), uint(TESSERA_GRADIENT_MAX_RECORDS) - 1u);
        color *= gradient_color(record, gradient_position(record, v_local));
    } else if (v_mode > 4000.0) {
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
