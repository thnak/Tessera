#version 450

// LCD subpixel variant of batched2d.frag (issue #471, slice 1 of docs/design/display-aware-rendering.md
// section 3.4): the same geometry, clipping and anti-aliasing program, plus the second fragment
// output the per-channel coverage blend needs.
//
// The blend is pipeline state, not shader state: a pipeline over this fragment must be created with
// `rhi::BlendMode::DualSource`, which on Vulkan is
//   srcColor/srgb = SRC1_COLOR, dstColor/dst = ONE_MINUS_SRC1_COLOR,
// per Vulkan's dual-source rules, giving `out_color * out_coverage + dst * (1 - out_coverage)` per
// channel (docs/design/gpu/rhi.md section 4). Writing the second output without that blend state
// would deposit raw coverage in the target instead.
//
// This variant is Vulkan-only. The window creates it when the device blends per channel, and the
// renderer binds it for glyph quads of runs the paint pass marked eligible (display-aware-
// rendering.md section 3.4.1). A glyph quad reads the atlas page's three subpixel coverages from
// the texel's R, G and B (or, with v_radius 1, its alpha as one coverage); every other quad
// replicates its scalar coverage across the channels, which is exactly the Alpha result.
//
// There is deliberately no TESSERA_SHADER_PORTABLE branch: WGSL has no output index, so the portable
// interface cannot express this shader, and the manifest entry declares no WebGPU source.

layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_color;
layout(location = 2) in vec2 v_local;
layout(location = 3) in vec2 v_half_size;
layout(location = 4) in float v_radius;
layout(location = 5) flat in float v_mode;
layout(location = 6) in vec4 v_carve;

layout(set = 0, binding = 0) uniform sampler2D u_texture;

// The clip-record buffer (ADR-068 slice 3), the same layout batched2d.frag declares: set 1, a
// read-only storage buffer holding the frame's clip records and the interned set ranges.
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

layout(set = 1, binding = 0, std430) readonly buffer ClipBuffer {
    uint record_count;
    uint header_reserved[3];
    ClipRecord records[TESSERA_CLIP_MAX_RECORDS];
    ClipSetRange sets[TESSERA_CLIP_MAX_SETS];
    vec2 vertices[TESSERA_CLIP_MAX_VERTICES]; // polygon vertex pool (ADR-068 slice 5)
} clip_buffer;

// Must match Batched2DPushConstants, the same block batched2d.frag uses (16 bytes): the LCD variant
// consumes the identical vertex data and push constants, so switching a run to it is a pipeline swap.
layout(push_constant) uniform PushConstants {
    vec2 viewport;
    float aa_radius;
    uint clip_set;
} pc;

// Output 0 is colour, output 1 is coverage; together they are the two sources of the dual-source
// blend. `layout(location = 0, index = 1)` is the Vulkan GLSL spelling of the second source.
layout(location = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_coverage;

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

// The blurred coverage of a rounded box at a point (inset shadows, issue #612): the box's own
// indicator convolved with a Gaussian of standard deviation `sigma`. The box is separable, so the
// x extent is two erf terms; the rounded corners shorten that extent where the row is within the
// corner radius of the top or bottom, and the y integral is taken numerically over +/- 3 sigma in
// eight midpoint steps (the method of Evan Wallace's "Fast Rounded Rectangle Shadows"). `p` is
// relative to the box centre. A signed-distance edge profile is exact for a straight edge only: at
// the corner of a box that is not much wider than the blur it is 35/255 off Chromium.
float blurred_box_x(float x, float y, float sigma, float corner, vec2 half_size) {
    float delta = min(half_size.y - corner - abs(y), 0.0);
    float curved = half_size.x - corner + sqrt(max(0.0, corner * corner - delta * delta));
    float k = 0.70710678 / sigma;
    return 0.5 * (erf_approx((x + curved) * k) - erf_approx((x - curved) * k));
}

float blurred_box(vec2 p, vec2 half_size, float corner, float sigma) {
    float low = p.y - half_size.y;
    float high = p.y + half_size.y;
    float start = clamp(-3.0 * sigma, low, high);
    float end = clamp(3.0 * sigma, low, high);
    float step_y = (end - start) / 8.0;
    float y = start + step_y * 0.5;
    float norm = 0.39894228 / sigma;
    float value = 0.0;
    for (int i = 0; i < 8; ++i) {
        float weight = norm * exp(-0.5 * y * y / (sigma * sigma));
        value += blurred_box_x(p.x, p.y - y, sigma, corner, half_size) * weight * step_y;
        y += step_y;
    }
    return clamp(value, 0.0, 1.0);
}

// Hard-edged clip test, used when AA is off (golden mode, aa_radius 0): the pixel-center rule keeps
// it deterministic and never writes outside the clip. It dispatches on the record's kind (ADR-068
// slice 4): an `Ellipse` record is a circle/ellipse, a `RoundedRect` is a rect with per-corner
// elliptical radii. The clip arrives by index, not by value: recent glslang (16.x, the Linux distro
// build) lowers a by-value struct parameter to a duplicated struct type plus OpCopyLogical, which
// naga's SPIR-V front end rejects, so the cross-compiled WGSL could not be produced on those hosts.
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

// Anti-aliased coverage of one clip: the clip's own signed distance blended over +/- aa_radius
// pixels. An `Ellipse` record uses the ellipse's own distance approximation; a `RoundedRect` uses
// the rounded-box distance with the corner radius that applies at the pixel's quadrant.
float clip_coverage(vec2 p, uint index) {
    if (clip_buffer.records[index].kind == 2u) {
        return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, polygon_signed_distance(p, index));
    }
    vec2 origin = clip_buffer.records[index].rect.xy;
    vec2 half_size = clip_buffer.records[index].rect.zw * 0.5;
    vec2 q = p - (origin + half_size);
    if (clip_buffer.records[index].kind == 1u) {
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

void main() {
    // Local units per device pixel, measured from the interpolated local coordinate (linear, so
    // the derivative is exact). It is 1.0 for an untransformed quad and 1/scale under a scale or
    // rotation, which keeps the SDF edge about aa_radius device pixels wide instead of aa_radius
    // local units (antialiasing.md section 5).
    vec2 local_dx = dFdx(v_local);
    vec2 local_dy = dFdy(v_local);
    float local_per_pixel = max(max(length(vec2(local_dx.x, local_dy.x)),
                                    length(vec2(local_dx.y, local_dy.y))), 1e-6);
    float edge_radius = pc.aa_radius * local_per_pixel;

    // Rounded clips are applied as a coverage mask: outside any clip alpha becomes 0 and the
    // alpha-blend leaves the destination untouched. This deliberately avoids `discard`, so the
    // shader does not require shaderDemoteToHelperInvocation (same policy as the SDF AA path).
    // With AA on the mask has anti-aliased coverage; with aa_radius 0 it is the hard test.
    // The set is a range into the record list (ADR-068 slice 3), so the loop bound is data, not a
    // constant: an unclipped draw has count 0 and the loop body never runs. The range is read field
    // by field rather than copied into a local, because a by-value struct copy lowers to
    // OpCopyLogical, which naga's SPIR-V front end rejects.
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
    const vec4 texel = texture(u_texture, v_uv);
    // Per-channel coverage of an LCD glyph quad (mode 2); negative means "no per-channel source, use
    // the scalar coverage". The atlas stores each pixel's three subpixel coverages in the texel's
    // R, G and B (display-aware-rendering.md section 3.4.1), so a text quad reads them directly.
    vec3 channel_coverage = vec3(-1.0);

    if (v_mode > 0.5 && v_mode < 1.5) {
        float d = sd_round_box(v_local, v_half_size, v_radius);
        float alpha;
        if (pc.aa_radius <= 0.0) {
            alpha = d <= 0.0 ? 1.0 : 0.0;
        } else {
            alpha = 1.0 - smoothstep(-edge_radius, edge_radius, d);
        }
        color.a *= alpha;
    } else if (v_mode > 1.5 && v_mode < 2.5) {
        vec4 texel_color = texel;
        texel_color.a = min(texel_color.a * (1.0 + (v_mode - 2.0) * 4.0), 1.0);
        color *= texel_color;
        // The same contrast boost the grayscale path applies to alpha, applied to each channel. A
        // quad with v_radius 1 is a run (or glyph) that must not carry fringes: it reads the page's
        // alpha - the strongest channel - as one coverage for all three, which is the Alpha result.
        float boost = 1.0 + (v_mode - 2.0) * 4.0;
        channel_coverage = v_radius > 0.5
                               ? vec3(min(texel.a * boost, 1.0)) * v_color.a
                               : min(texel.rgb * boost, vec3(1.0)) * v_color.a;
    } else if (v_mode > 2000.0) {
        // This test comes BEFORE the outer shadow's (mode > 1000): an inset mode (2048 + sigma) is
        // also above 1000, so the other order sent every inset shadow down the outer-shadow path.
        // Inset box shadow (issue #612): mode is 2048 + sigma. The quad is the element's border box
        // and `v_carve` is the inner box, so the shadow fills the ring between them and fades
        // inward. See batched2d.frag for the derivation.
        float sigma = v_mode - 2048.0;
        vec2 inner_half = v_carve.z > 0.0 && v_carve.w > 0.0 ? v_carve.zw : v_half_size;
        vec2 inner_centre = v_carve.z > 0.0 && v_carve.w > 0.0 ? v_carve.xy : vec2(0.0);
        float inset = v_half_size.x - inner_half.x;
        float inner_radius = clamp(v_radius - inset, 0.0, min(inner_half.x, inner_half.y));
        float d = sd_round_box(v_local - inner_centre, inner_half, inner_radius);
        float cover;
        if (sigma > 0.0) {
            cover = 1.0 - blurred_box(v_local - inner_centre, inner_half, inner_radius, sigma);
        } else if (pc.aa_radius <= 0.0) {
            cover = d > 0.0 ? 1.0 : 0.0;
        } else {
            cover = smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        float outer = sd_round_box(v_local, v_half_size, v_radius);
        if (pc.aa_radius <= 0.0) {
            cover = outer <= 0.0 ? cover : 0.0;
        } else {
            cover *= 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, outer);
        }
        channel_coverage = vec3(cover) * v_color.a;
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

    // Colour to output 0 and coverage to output 1 (the two sources of the dual-source blend).
    // The colour's own alpha stays 1: the destination alpha resolves to `coverage + dst * (1 -
    // coverage)` with the alpha channel's coverage being the strongest of the three, the same "how
    // much ink" reduction the atlas stores in alpha. A quad with no per-channel source (anything
    // that is not a glyph) replicates its scalar coverage, which is exactly the Alpha result.
    // Output 0 is the ink as authored (v_color), not `color`: the text branch above multiplies
    // `color` by the texel for the scalar path, and for an LCD page the texel's RGB is coverage,
    // which output 1 already applies per channel.
    out_color = vec4(v_color.rgb, 1.0);
    if (channel_coverage.x >= 0.0) {
        vec3 covered = channel_coverage * clip_alpha;
        out_coverage = vec4(covered, max(covered.r, max(covered.g, covered.b)));
    } else {
        out_coverage = vec4(color.a);
    }
}
