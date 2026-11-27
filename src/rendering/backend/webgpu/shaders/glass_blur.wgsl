// WGSL twin of ../../vulkan/shaders/glass_blur.frag (ADR-011: two sources, one manifest entry).
// WebGPU has no push constants, so the 112-byte block is a uniform buffer at group 0, binding 0 and
// the combined image sampler splits into a texture at binding 1 and a sampler at binding 2, the
// convention batched2d.wgsl uses. Authored directly, never translated at run time; validated by
// naga (tests) and the Wasm CI lane, which is the only place the WebGPU C++ path is compiled.
//
// The tap loop mirrors `glass_kernel_taps` in src/rendering/gpu/glass_blur.cpp: adjacent texel
// pairs collapse into one linear sample (radius + 1 taps), the weights are normalized by their sum,
// and the sampler is clamp to edge.

struct Params {
    texel_step: vec2<f32>,
    direction: vec2<f32>,
    sigma: f32,
    radius: f32,
    region_origin: vec2<f32>,
    region_size: vec2<f32>,
    box: vec4<f32>,   // xy = box origin, zw = box size (destination space)
    radii: vec4<f32>, // top-left, top-right, bottom-right, bottom-left
    factors: vec3<f32>, // saturate, brightness, contrast (vertical pass only)
    mode: f32,        // 0 = horizontal into the blur target, 1 = vertical + composite
    opacity: f32,     // node opacity, folded into the composite alpha (vertical pass only)
};

@group(0) @binding(0) var<uniform> pc: Params;
@group(0) @binding(1) var u_texture: texture_2d<f32>;
@group(0) @binding(2) var u_sampler: sampler;

// Fullscreen triangle from the vertex index, like the GLSL fullscreen.vert. WebGPU clip space is
// y-up, Vulkan's is y-down; the triangle covers the target either way.
@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4<f32> {
    let p = vec2<f32>(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
}

fn sample_at(uv: vec2<f32>) -> vec4<f32> {
    return textureSampleLevel(u_texture, u_sampler, uv, 0.0);
}

// The CSS colour filter functions on a straight-alpha pixel (mode 3), mirrored from
// include/tessera/rendering/gpu/color_filter.hpp. The ids are the ColorFunction enum of
// layer_filter.hpp. Matrices are column-major: each column holds one input channel's coefficients.
fn apply_color_function(id: i32, amount: f32, px: vec4<f32>) -> vec4<f32> {
    var c = px.rgb;
    var a = px.a;
    if (id == 1) {
        c = c * amount;
    } else if (id == 2) {
        c = c * amount + vec3<f32>((1.0 - amount) * 0.5);
    } else if (id == 3) {
        let k = 1.0 - clamp(amount, 0.0, 1.0);
        c = mat3x3<f32>(
            vec3<f32>(0.2126 + 0.7874 * k, 0.2126 - 0.2126 * k, 0.2126 - 0.2126 * k),
            vec3<f32>(0.7152 - 0.7152 * k, 0.7152 + 0.2848 * k, 0.7152 - 0.7152 * k),
            vec3<f32>(0.0722 - 0.0722 * k, 0.0722 - 0.0722 * k, 0.0722 + 0.9278 * k)) * c;
    } else if (id == 4) {
        let k = 1.0 - clamp(amount, 0.0, 1.0);
        c = mat3x3<f32>(
            vec3<f32>(0.393 + 0.607 * k, 0.349 - 0.349 * k, 0.272 - 0.272 * k),
            vec3<f32>(0.769 - 0.769 * k, 0.686 + 0.314 * k, 0.534 - 0.534 * k),
            vec3<f32>(0.189 - 0.189 * k, 0.168 - 0.168 * k, 0.131 + 0.869 * k)) * c;
    } else if (id == 5) {
        let s = amount;
        c = mat3x3<f32>(
            vec3<f32>(0.213 + 0.787 * s, 0.213 - 0.213 * s, 0.213 - 0.213 * s),
            vec3<f32>(0.715 - 0.715 * s, 0.715 + 0.285 * s, 0.715 - 0.715 * s),
            vec3<f32>(0.072 - 0.072 * s, 0.072 - 0.072 * s, 0.072 + 0.928 * s)) * c;
    } else if (id == 6) {
        let co = cos(amount);
        let si = sin(amount);
        c = mat3x3<f32>(
            vec3<f32>(0.213 + co * 0.787 - si * 0.213, 0.213 - co * 0.213 + si * 0.143,
                      0.213 - co * 0.213 - si * 0.787),
            vec3<f32>(0.715 - co * 0.715 - si * 0.715, 0.715 + co * 0.285 + si * 0.140,
                      0.715 - co * 0.715 + si * 0.715),
            vec3<f32>(0.072 - co * 0.072 + si * 0.928, 0.072 - co * 0.072 - si * 0.283,
                      0.072 + co * 0.928 + si * 0.072)) * c;
    } else if (id == 7) {
        let k = clamp(amount, 0.0, 1.0);
        c = c * (1.0 - 2.0 * k) + vec3<f32>(k);
    } else if (id == 8) {
        a = a * clamp(amount, 0.0, 1.0);
    } else {
        return px;
    }
    return clamp(vec4<f32>(c, a), vec4<f32>(0.0), vec4<f32>(1.0));
}

fn pair_weight(offset: f32, sigma: f32) -> f32 {
    return exp(-(offset * offset) / (2.0 * sigma * sigma));
}

fn corner_radius(local: vec2<f32>, radii: vec4<f32>) -> f32 {
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

fn sd_round_box(p: vec2<f32>, half_size: vec2<f32>, radius: f32) -> f32 {
    let q = abs(p) - half_size + vec2<f32>(radius);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0))) - radius;
}

@fragment
fn fs_main(@builtin(position) position: vec4<f32>) -> @location(0) vec4<f32> {
    let frag = position.xy;
    var uv = frag * pc.texel_step;
    if (pc.mode >= 0.5 && pc.mode <= 2.5) {
        uv = (frag - pc.region_origin) / max(pc.region_size, vec2<f32>(1.0));
    }

    var blurred = sample_at(uv);
    if (pc.sigma > 0.0) {
        var total = 0.0;
        var sum = vec3<f32>(0.0);
        var alpha = 0.0;
        let radius = i32(pc.radius);
        for (var tap = 0; tap <= radius; tap = tap + 1) {
            let a = f32(2 * tap) - pc.radius;
            let b = min(a + 1.0, pc.radius);
            let wa = pair_weight(a, pc.sigma);
            let wb = select(0.0, pair_weight(b, pc.sigma), b > a);
            let weight = wa + wb;
            // The unpaired tap keeps its exact integer offset (see glass_kernel_taps()).
            let offset = select(a, (a * wa + b * wb) / max(weight, 1.0e-8), b > a);
            let sample = sample_at(uv + pc.direction * pc.texel_step * offset);
            sum = sum + sample.rgb * weight;
            alpha = alpha + sample.a * weight;
            total = total + weight;
        }
        blurred = vec4<f32>(sum / total, alpha / total);
    }

    if (pc.mode < 0.5) {
        return blurred;
    }

    // mode 3: colour filter functions over a layer (issue #617): ids in `radii`, amounts in
    // `factors`, id 0 ends the list; the output is premultiplied again. See glass_blur.frag.
    if (pc.mode > 2.5) {
        let layer_a = blurred.a;
        var px = vec4<f32>(select(vec3<f32>(0.0), blurred.rgb / max(layer_a, 1.0e-8), layer_a > 0.0), layer_a);
        px = apply_color_function(i32(pc.radii.x + 0.5), pc.factors.x, px);
        px = apply_color_function(i32(pc.radii.y + 0.5), pc.factors.y, px);
        px = apply_color_function(i32(pc.radii.z + 0.5), pc.factors.z, px);
        return vec4<f32>(px.rgb * px.a, px.a);
    }

    // mode 2: composite an isolated layer (issue #227): premultiplied layer colour back to straight
    // alpha, the node's opacity applied once to the group, no colour matrix and no rounded clip (the
    // scissor bounds the composite). See glass_blur.frag.
    if (pc.mode > 1.5) {
        let layer_alpha = blurred.a;
        var straight = vec3<f32>(0.0);
        if (layer_alpha > 0.0) {
            straight = blurred.rgb / layer_alpha;
        }
        return vec4<f32>(straight, layer_alpha * pc.opacity);
    }

    // Vertical pass: color matrix (saturate, then brightness, then contrast, as CSS composes them)
    // and the rounded-box composite clip of the paint pass.
    var rgb = blurred.rgb;
    let luminance = dot(rgb, vec3<f32>(0.2126, 0.7152, 0.0722));
    rgb = vec3<f32>(luminance) + (rgb - vec3<f32>(luminance)) * pc.factors.x;
    rgb = rgb * pc.factors.y;
    rgb = (rgb - vec3<f32>(0.5)) * pc.factors.z + vec3<f32>(0.5);
    rgb = clamp(rgb, vec3<f32>(0.0), vec3<f32>(1.0));

    let local = frag - pc.box.xy - pc.box.zw * 0.5;
    let radius = corner_radius(local, pc.radii);
    let distance = sd_round_box(local, pc.box.zw * 0.5, radius);
    let coverage = clamp(0.5 - distance, 0.0, 1.0);
    // The composite pass draws over the frame target with straight-alpha blending: this alpha is the
    // blend weight (opacity * blurred + (1 - opacity) * original for an opaque backdrop) and
    // `coverage` antialiases the rounded clip. The horizontal pass writes straight alpha unblended.
    return vec4<f32>(rgb, blurred.a * coverage * pc.opacity);
}
