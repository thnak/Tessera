// WGSL twin of ../../vulkan/shaders/batched2d.{vert,frag} (ADR-011: two sources, one manifest).
// WebGPU has no push constants, so the 112-byte block is a uniform buffer at group 0, binding 0
// (Batched2DPushConstants layout: viewport, aa_radius, clip_count, 3 clips of rect + radii).
// Unbuilt against a real WebGPU implementation on the authoring host; verify in the Wasm CI lane.

struct ClipUniform {
    rect: vec4<f32>,  // xy = origin, zw = size in target pixels
    radii: vec4<f32>, // top-left, top-right, bottom-right, bottom-left
};

struct Params {
    viewport: vec2<f32>,
    aa_radius: f32,
    clip_count: u32,
    clips: array<ClipUniform, 3>,
};

@group(0) @binding(0) var<uniform> pc: Params;
@group(0) @binding(1) var u_texture: texture_2d<f32>;
@group(0) @binding(2) var u_sampler: sampler;

struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) color: vec4<f32>,
    @location(3) local: vec2<f32>,
    @location(4) half_size: vec2<f32>,
    @location(5) radius: f32,
    @location(6) mode: f32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
    @location(2) local: vec2<f32>,
    @location(3) half_size: vec2<f32>,
    @location(4) radius: f32,
    @location(5) @interpolate(flat) mode: f32,
};

@vertex
fn vs_main(in: VertexInput) -> VertexOutput {
    var out: VertexOutput;
    // Pixel space has y down; WebGPU clip space has y up (Vulkan's is y down), so flip y here.
    let ndc = vec2<f32>(in.position.x / pc.viewport.x * 2.0 - 1.0,
                        1.0 - in.position.y / pc.viewport.y * 2.0);
    out.position = vec4<f32>(ndc, 0.0, 1.0);
    out.uv = in.uv;
    out.color = in.color;
    out.local = in.local;
    out.half_size = in.half_size;
    out.radius = in.radius;
    out.mode = in.mode;
    return out;
}

fn sd_round_box(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
    let q = abs(p) - b + vec2<f32>(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0))) - r;
}

fn inside_clip(p: vec2<f32>, clip: ClipUniform) -> bool {
    let origin = clip.rect.xy;
    let size = clip.rect.zw;
    if (p.x < origin.x || p.y < origin.y || p.x > origin.x + size.x || p.y > origin.y + size.y) {
        return false;
    }
    let q = p - origin;
    if (q.x < clip.radii.x && q.y < clip.radii.x) {
        let d = vec2<f32>(q.x - clip.radii.x, q.y - clip.radii.x);
        return dot(d, d) <= clip.radii.x * clip.radii.x;
    }
    if (q.x > size.x - clip.radii.y && q.y < clip.radii.y) {
        let d = vec2<f32>(q.x - (size.x - clip.radii.y), q.y - clip.radii.y);
        return dot(d, d) <= clip.radii.y * clip.radii.y;
    }
    if (q.x > size.x - clip.radii.z && q.y > size.y - clip.radii.z) {
        let d = vec2<f32>(q.x - (size.x - clip.radii.z), q.y - (size.y - clip.radii.z));
        return dot(d, d) <= clip.radii.z * clip.radii.z;
    }
    if (q.x < clip.radii.w && q.y > size.y - clip.radii.w) {
        let d = vec2<f32>(q.x - clip.radii.w, q.y - (size.y - clip.radii.w));
        return dot(d, d) <= clip.radii.w * clip.radii.w;
    }
    return true;
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // Coverage-mask clipping instead of discard, matching the GLSL shader.
    var clip_alpha = 1.0;
    for (var index = 0u; index < pc.clip_count; index = index + 1u) {
        if (!inside_clip(in.position.xy, pc.clips[index])) {
            clip_alpha = 0.0;
        }
    }

    var color = in.color;
    // textureSample needs uniform control flow, so sample unconditionally and select.
    let sampled = textureSample(u_texture, u_sampler, in.uv);
    if (in.mode > 0.5 && in.mode < 1.5) {
        let d = sd_round_box(in.local, in.half_size, in.radius);
        var alpha = 0.0;
        if (pc.aa_radius <= 0.0) {
            alpha = select(0.0, 1.0, d <= 0.0);
        } else {
            alpha = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        color.a = color.a * alpha;
    } else if (in.mode > 1.5) {
        color = color * sampled;
    }
    color.a = color.a * clip_alpha;
    return color;
}
