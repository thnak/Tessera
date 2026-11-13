#version 450
// SPDX-License-Identifier: MIT
//
// Glass backdrop blur (docs/specs/glass-v1.md section 5), one pass of the separable Gaussian. Two
// dispatches per glass node:
//   mode 0: horizontal blur of the backdrop copy into a pooled blur target;
//   mode 1: vertical blur into the frame target, plus the saturate/brightness/contrast color matrix
//           and the node's rounded-box composite clip (so the effect adds no third pass).
//
// Geometry is the fullscreen triangle of `fullscreen.vert` (`gl_VertexIndex`, no vertex buffer);
// `gl_FragCoord` is the only coordinate input, so the direction, sigma and region are one
// push-constant block (a std140 uniform buffer in the portable interface, ADR-049).
//
// The tap loop mirrors `glass_kernel_taps` in src/rendering/gpu/glass_blur.cpp exactly: adjacent
// texel pairs collapse into one linear sample (radius + 1 taps instead of 2 * radius + 1), the
// weights are normalized by their sum, and the sampler is clamp to edge so a glass node at a window
// border does not darken.

#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 1) uniform texture2D u_texture;
layout(set = 0, binding = 2) uniform sampler u_sampler;
#define TESSERA_SAMPLE(uv) texture(sampler2D(u_texture, u_sampler), (uv))
#else
layout(set = 0, binding = 0) uniform sampler2D u_texture;
#define TESSERA_SAMPLE(uv) texture(u_texture, (uv))
#endif

// The block below is the whole interface (72 bytes of fields, 80 with std140 padding). The portable
// interface turns it into a std140 uniform buffer at binding 0.
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 0, std140) uniform PushConstants {
#else
layout(push_constant) uniform PushConstants {
#endif
    vec2 texel_step;    // 1 / blur-target extent
    vec2 direction;     // (1, 0) horizontal, (0, 1) vertical
    float sigma;        // Gaussian sigma in blur-target texels
    float radius;       // ceil(3 * sigma), the same integer radius the CPU kernel uses
    vec2 region_origin; // destination-space origin of the blurred region
    vec2 region_size;   // destination-space size of the blurred region
    vec4 box;           // xy = composite box origin, zw = box size (destination space)
    vec4 radii;         // top-left, top-right, bottom-right, bottom-left corner radii
    vec3 factors;       // saturate, brightness, contrast (vertical pass only)
    float mode;         // 0 = horizontal into the blur target, 1 = vertical + composite
} pc;

layout(location = 0) out vec4 out_color;

float pair_weight(float offset, float sigma) {
    return exp(-(offset * offset) / (2.0 * sigma * sigma));
}

float corner_radius(vec2 local, vec4 radii) {
    if (local.x < 0.0 && local.y < 0.0) {
        return radii.x;
    }
    if (local.x >= 0.0 && local.y < 0.0) {
        return radii.y;
    }
    if (local.x >= 0.0 && local.y >= 0.0) {
        return radii.z;
    }
    return radii.w;
}

float sd_round_box(vec2 p, vec2 half_size, float radius) {
    vec2 q = abs(p) - half_size + vec2(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2(0.0))) - radius;
}

// One direction of the separable Gaussian, at the destination pixel's normalized position. The
// horizontal pass renders into a blur target as large as the region (or half of it); the vertical
// pass renders into the frame target, so its uv is mapped from the region placement.
void main() {
    vec2 uv = pc.mode < 0.5
                  ? gl_FragCoord.xy * pc.texel_step
                  : (gl_FragCoord.xy - pc.region_origin) / max(pc.region_size, vec2(1.0));

    vec4 blurred = TESSERA_SAMPLE(uv);
    if (pc.sigma > 0.0) {
        float total = 0.0;
        vec3 sum = vec3(0.0);
        float alpha = 0.0;
        for (int tap = 0; tap <= int(pc.radius); ++tap) {
            float a = float(2 * tap) - pc.radius;
            float b = min(a + 1.0, pc.radius);
            float wa = pair_weight(a, pc.sigma);
            float wb = b > a ? pair_weight(b, pc.sigma) : 0.0;
            float weight = wa + wb;
            // The unpaired tap keeps its exact integer offset (see glass_kernel_taps()).
            float offset = b > a ? (a * wa + b * wb) / max(weight, 1.0e-8) : a;
            vec4 texel = TESSERA_SAMPLE(uv + pc.direction * pc.texel_step * offset);
            sum += texel.rgb * weight;
            alpha += texel.a * weight;
            total += weight;
        }
        blurred = vec4(sum / total, alpha / total);
    }

    if (pc.mode < 0.5) {
        out_color = blurred;
        return;
    }

    // Vertical pass: color matrix (saturate, then brightness, then contrast, as CSS composes them)
    // and the rounded-box composite clip of the paint pass.
    vec3 rgb = blurred.rgb;
    float luminance = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    rgb = vec3(luminance) + (rgb - vec3(luminance)) * pc.factors.x;
    rgb *= pc.factors.y;
    rgb = (rgb - vec3(0.5)) * pc.factors.z + vec3(0.5);
    rgb = clamp(rgb, vec3(0.0), vec3(1.0));

    vec2 local = gl_FragCoord.xy - pc.box.xy - pc.box.zw * 0.5;
    float radius = corner_radius(local, pc.radii);
    float distance = sd_round_box(local, pc.box.zw * 0.5, radius);
    // One-pixel analytic edge, matching the paint pass's rounded clip.
    float coverage = clamp(0.5 - distance, 0.0, 1.0);
    out_color = vec4(rgb, blurred.a * coverage);
}
