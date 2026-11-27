#version 450
// SPDX-License-Identifier: MIT
//
// Glass backdrop blur (docs/specs/glass-v1.md section 5), one pass of the separable Gaussian. Two
// dispatches per glass node:
//   mode 0: horizontal blur of the backdrop copy into a pooled blur target;
//   mode 1: vertical blur into the frame target, plus the saturate/brightness/contrast color matrix
//           and the node's rounded-box composite clip (so the effect adds no third pass);
//   mode 2: isolated-layer composite (issue #227);
//   mode 3: CSS colour filter functions over a layer (issue #617), up to three, in order.
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

// The block below is the whole interface (76 bytes of fields, 112 with padding). The portable
// interface turns it into a std140 uniform buffer at binding 0.
#ifdef TESSERA_SHADER_PORTABLE
layout(set = 0, binding = 0, std140) uniform PushConstants {
#else
layout(push_constant) uniform PushConstants {
#endif
    vec2 texel_step;    // 1 / blur-target extent (source-space step for the horizontal pass)
    vec2 direction;     // (1, 0) horizontal, (0, 1) vertical
    float sigma;        // Gaussian sigma in blur-target texels
    float radius;       // ceil(3 * sigma), the same integer radius the CPU kernel uses
    vec2 region_origin; // destination-space origin of the blurred region
    vec2 region_size;   // destination-space size of the blurred region
    vec4 box;           // xy = composite box origin, zw = box size (destination space)
    vec4 radii;         // top-left, top-right, bottom-right, bottom-left corner radii
    vec3 factors;       // saturate, brightness, contrast (vertical pass only)
    float mode;         // 0 = horizontal into the blur target, 1 = vertical + composite
    float opacity;      // node opacity, folded into the composite alpha (vertical pass only)
} pc;

layout(location = 0) out vec4 out_color;

// The colour filter functions of CSS Filter Effects on a straight-alpha pixel, mirrored from
// include/tessera/rendering/gpu/color_filter.hpp (apply_color_function): same coefficients, same
// clamp after every function. The ids are the ColorFunction enum of layer_filter.hpp.
vec4 apply_color_function(int id, float amount, vec4 px) {
    vec3 c = px.rgb;
    float a = px.a;
    if (id == 1) { // brightness
        c *= amount;
    } else if (id == 2) { // contrast
        c = c * amount + vec3((1.0 - amount) * 0.5);
    } else if (id == 3) { // grayscale
        float k = 1.0 - clamp(amount, 0.0, 1.0);
        c = mat3(0.2126 + 0.7874 * k, 0.2126 - 0.2126 * k, 0.2126 - 0.2126 * k,
                 0.7152 - 0.7152 * k, 0.7152 + 0.2848 * k, 0.7152 - 0.7152 * k,
                 0.0722 - 0.0722 * k, 0.0722 - 0.0722 * k, 0.0722 + 0.9278 * k) * c;
    } else if (id == 4) { // sepia
        float k = 1.0 - clamp(amount, 0.0, 1.0);
        c = mat3(0.393 + 0.607 * k, 0.349 - 0.349 * k, 0.272 - 0.272 * k,
                 0.769 - 0.769 * k, 0.686 + 0.314 * k, 0.534 - 0.534 * k,
                 0.189 - 0.189 * k, 0.168 - 0.168 * k, 0.131 + 0.869 * k) * c;
    } else if (id == 5) { // saturate
        float s = amount;
        c = mat3(0.213 + 0.787 * s, 0.213 - 0.213 * s, 0.213 - 0.213 * s,
                 0.715 - 0.715 * s, 0.715 + 0.285 * s, 0.715 - 0.715 * s,
                 0.072 - 0.072 * s, 0.072 - 0.072 * s, 0.072 + 0.928 * s) * c;
    } else if (id == 6) { // hue-rotate, amount in radians
        float co = cos(amount);
        float si = sin(amount);
        c = mat3(0.213 + co * 0.787 - si * 0.213, 0.213 - co * 0.213 + si * 0.143,
                 0.213 - co * 0.213 - si * 0.787,
                 0.715 - co * 0.715 - si * 0.715, 0.715 + co * 0.285 + si * 0.140,
                 0.715 - co * 0.715 + si * 0.715,
                 0.072 - co * 0.072 + si * 0.928, 0.072 - co * 0.072 - si * 0.283,
                 0.072 + co * 0.928 + si * 0.072) * c;
    } else if (id == 7) { // invert
        float k = clamp(amount, 0.0, 1.0);
        c = c * (1.0 - 2.0 * k) + vec3(k);
    } else if (id == 8) { // opacity
        a *= clamp(amount, 0.0, 1.0);
    } else {
        return px;
    }
    return clamp(vec4(c, a), vec4(0.0), vec4(1.0));
}

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
    // Modes 0 and 3 read their source 1:1 through `texel_step`; the others map the region placement.
    vec2 uv = (pc.mode < 0.5 || pc.mode > 2.5)
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

    // mode 3: colour filter functions over a layer (issue #617). The pass is a plain 1:1 copy with a
    // per-pixel function, through the non-blending pipeline into a transparent target, so the
    // output is the layer's premultiplied form again. The function ids ride in `radii` and their
    // amounts in `factors` (the other fields are unused in this mode); id 0 ends the list.
    if (pc.mode > 2.5) {
        float alpha = blurred.a;
        vec4 px = vec4(alpha > 0.0 ? blurred.rgb / alpha : vec3(0.0), alpha);
        for (int i = 0; i < 3; ++i) {
            px = apply_color_function(int(pc.radii[i] + 0.5), pc.factors[i], px);
        }
        out_color = vec4(px.rgb * px.a, px.a);
        return;
    }

    // mode 2: composite an isolated layer (issue #227). The layer target holds what the batched
    // pipeline's straight-alpha blend accumulated over a transparent clear, which is premultiplied
    // colour with a correct alpha, so the colour is divided back to straight alpha and the node's
    // opacity is applied once to the group. The composite draws over the frame with the same
    // straight-alpha blend as mode 1: result = layer.rgb * opacity + frame * (1 - layer.a * opacity).
    // No colour matrix and no rounded clip: the layer's own content is already shaped, and the
    // scissor bounds the composite.
    if (pc.mode > 1.5) {
        float layer_alpha = blurred.a;
        vec3 straight = layer_alpha > 0.0 ? blurred.rgb / layer_alpha : vec3(0.0);
        out_color = vec4(straight, layer_alpha * pc.opacity);
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
    // The composite pass draws over the frame target with straight-alpha blending, so this alpha is
    // the blend weight: `opacity * blurred + (1 - opacity) * original` for an opaque backdrop, with
    // `coverage` antialiasing the rounded clip. The horizontal pass writes straight alpha with no
    // blending (the blur target only feeds this pass).
    out_color = vec4(rgb, blurred.a * coverage * pc.opacity);
}
