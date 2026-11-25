// WGSL twin of ../../vulkan/shaders/batched2d.{vert,frag} (ADR-011: two sources, one manifest).
// WebGPU has no push constants, so the 16-byte block is a uniform buffer at group 0, binding 0
// (Batched2DPushConstants layout: viewport, aa_radius, clip_set).
//
// The clip records live in a read-only storage buffer at group 0, binding 3 (ADR-068 slice 3), the
// same layout the Vulkan shader reads from set 1. WebGPU's fragment stage allows 8 storage buffers,
// so one is not a constraint.
// Unbuilt against a real WebGPU implementation on the authoring host; verify in the Wasm CI lane.

const TESSERA_CLIP_MAX_RECORDS: u32 = 256u;
const TESSERA_CLIP_MAX_SETS: u32 = 256u;

struct ClipRecord {
    kind: u32,
    flags: u32,
    first_vertex: u32,
    vertex_count: u32,
    rect: vec4<f32>,    // xy = origin, zw = size in target pixels
    radii_a: vec4<f32>, // top-left, top-right, bottom-right, bottom-left
    radii_b: vec4<f32>, // elliptical second components; zero for a circular corner
};

struct ClipSetRange {
    first_record: u32,
    count: u32,
    reserved0: u32,
    reserved1: u32,
};

// The header is four u32s (16 bytes, ClipBufferHeader), so the records start at offset 16 as in the
// Vulkan std430 layout. A `vec3<u32>` here would align to 16 and push every record to offset 32,
// so the shader would read the wrong bytes and the struct would be 16 bytes larger than the buffer.
struct ClipBuffer {
    record_count: u32,
    header_reserved0: u32,
    header_reserved1: u32,
    header_reserved2: u32,
    records: array<ClipRecord, TESSERA_CLIP_MAX_RECORDS>,
    sets: array<ClipSetRange, TESSERA_CLIP_MAX_SETS>,
};

struct Params {
    viewport: vec2<f32>,
    aa_radius: f32,
    clip_set: u32,
};

@group(0) @binding(0) var<uniform> pc: Params;
@group(0) @binding(1) var u_texture: texture_2d<f32>;
@group(0) @binding(2) var u_sampler: sampler;
@group(0) @binding(3) var<storage, read> clip_buffer: ClipBuffer;

struct VertexInput {
    @location(0) position: vec2<f32>,
    @location(1) uv: vec2<f32>,
    @location(2) color: vec4<f32>,
    @location(3) local: vec2<f32>,
    @location(4) half_size: vec2<f32>,
    @location(5) radius: f32,
    @location(6) mode: f32,
    @location(7) carve: vec4<f32>,
    // Index into the sampled-image array (RHI 1.7, issue #480 part 2). WebGPU has no descriptor
    // indexing, so this twin always reads the single bound texture and ignores the index; the
    // attribute stays in the layout so one Vertex2D serves both backends.
    @location(8) texture_index: u32,
};

struct VertexOutput {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
    @location(1) color: vec4<f32>,
    @location(2) local: vec2<f32>,
    @location(3) half_size: vec2<f32>,
    @location(4) radius: f32,
    @location(5) @interpolate(flat) mode: f32,
    @location(6) carve: vec4<f32>,
    @location(7) @interpolate(flat) texture_index: u32,
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
    out.carve = in.carve;
    out.texture_index = in.texture_index;
    return out;
}

fn sd_round_box(p: vec2<f32>, b: vec2<f32>, r: f32) -> f32 {
    let q = abs(p) - b + vec2<f32>(r);
    return min(max(q.x, q.y), 0.0) + length(max(q, vec2<f32>(0.0))) - r;
}

fn inside_ellipse_record(p: vec2<f32>, index: u32) -> bool {
    let clip = clip_buffer.records[index];
    let half_size = clip.rect.zw * 0.5;
    if (half_size.x <= 0.0 || half_size.y <= 0.0) {
        return false;
    }
    let d = (p - (clip.rect.xy + half_size)) / half_size;
    return dot(d, d) <= 1.0;
}

fn inside_clip(p: vec2<f32>, index: u32) -> bool {
    let clip = clip_buffer.records[index];
    // Kind 1 is `Ellipse` (ADR-068 slice 4); the rest is the rounded-rect test.
    if (clip.kind == 1u) {
        return inside_ellipse_record(p, index);
    }
    let origin = clip.rect.xy;
    let size = clip.rect.zw;
    if (p.x < origin.x || p.y < origin.y || p.x > origin.x + size.x || p.y > origin.y + size.y) {
        return false;
    }
    let q = p - origin;
    // A zero vertical component means a circular corner: use the horizontal radius for both axes.
    let vertical = vec4<f32>(
        select(clip.radii_a.x, clip.radii_b.x, clip.radii_b.x > 0.0),
        select(clip.radii_a.y, clip.radii_b.y, clip.radii_b.y > 0.0),
        select(clip.radii_a.z, clip.radii_b.z, clip.radii_b.z > 0.0),
        select(clip.radii_a.w, clip.radii_b.w, clip.radii_b.w > 0.0));
    if (clip.radii_a.x > 0.0 && q.x < clip.radii_a.x && q.y < vertical.x) {
        let d = vec2<f32>((q.x - clip.radii_a.x) / clip.radii_a.x,
                          (q.y - vertical.x) / vertical.x);
        return dot(d, d) <= 1.0;
    }
    if (clip.radii_a.y > 0.0 && q.x > size.x - clip.radii_a.y && q.y < vertical.y) {
        let d = vec2<f32>((q.x - (size.x - clip.radii_a.y)) / clip.radii_a.y,
                          (q.y - vertical.y) / vertical.y);
        return dot(d, d) <= 1.0;
    }
    if (clip.radii_a.z > 0.0 && q.x > size.x - clip.radii_a.z && q.y > size.y - vertical.z) {
        let d = vec2<f32>((q.x - (size.x - clip.radii_a.z)) / clip.radii_a.z,
                          (q.y - (size.y - vertical.z)) / vertical.z);
        return dot(d, d) <= 1.0;
    }
    if (clip.radii_a.w > 0.0 && q.x < clip.radii_a.w && q.y > size.y - vertical.w) {
        let d = vec2<f32>((q.x - clip.radii_a.w) / clip.radii_a.w,
                          (q.y - (size.y - vertical.w)) / vertical.w);
        return dot(d, d) <= 1.0;
    }
    return true;
}

// erf, Abramowitz and Stegun 7.1.26 (see batched2d.frag).
fn erf_approx(x: f32) -> f32 {
    let a = abs(x);
    let t = 1.0 / (1.0 + 0.3275911 * a);
    let poly = t * (0.254829592 + t * (-0.284496736 + t * (1.421413741 + t * (-1.453152027 + t * 1.061405429))));
    let y = 1.0 - poly * exp(-a * a);
    return select(y, -y, x < 0.0);
}

fn clip_coverage(p: vec2<f32>, index: u32) -> f32 {
    let clip = clip_buffer.records[index];
    // See batched2d.frag: the clip's own signed distance over +/- aa_radius pixels.
    let half_size = clip.rect.zw * 0.5;
    let q = p - (clip.rect.xy + half_size);
    if (clip.kind == 1u) {
        if (half_size.x <= 0.0 || half_size.y <= 0.0) {
            return 0.0;
        }
        let d = q / half_size;
        let scale = min(half_size.x, half_size.y);
        let distance = (length(d) - 1.0) * scale;
        return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, distance);
    }
    var r = select(select(clip.radii_a.w, clip.radii_a.x, q.y < 0.0),
                   select(clip.radii_a.z, clip.radii_a.y, q.y < 0.0), q.x >= 0.0);
    r = min(r, min(half_size.x, half_size.y));
    let d = sd_round_box(q, half_size, r);
    return 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4<f32> {
    // Local units per device pixel (see batched2d.frag), taken before any branch so the derivative
    // stays in uniform control flow.
    let local_dx = dpdx(in.local);
    let local_dy = dpdy(in.local);
    let local_per_pixel = max(max(length(vec2<f32>(local_dx.x, local_dy.x)),
                                  length(vec2<f32>(local_dx.y, local_dy.y))), 1e-6);
    let edge_radius = pc.aa_radius * local_per_pixel;

    // Coverage-mask clipping instead of discard, matching the GLSL shader: hard test with AA off,
    // anti-aliased coverage with AA on. The set is a range into the record list (ADR-068 slice 3),
    // so an unclipped draw has count 0 and the loop body never runs.
    var clip_alpha = 1.0;
    let first_record = clip_buffer.sets[pc.clip_set].first_record;
    let clip_count = clip_buffer.sets[pc.clip_set].count;
    for (var offset = 0u; offset < clip_count; offset = offset + 1u) {
        let index = first_record + offset;
        if (pc.aa_radius <= 0.0) {
            if (!inside_clip(in.position.xy, index)) {
                clip_alpha = 0.0;
            }
        } else {
            clip_alpha = clip_alpha * clip_coverage(in.position.xy, index);
        }
    }

    var color = in.color;
    // textureSample needs uniform control flow, so sample unconditionally and select.
    let sampled = textureSample(u_texture, u_sampler, in.uv);
    if (in.mode > 4000.0) {
        // Exact nearest texture (#682, kExactNearestMode; see batched2d.frag): integer math on the
        // fragment's device pixel, so every GPU agrees with the CPU rule. `carve` is the integer
        // device rectangle (xy origin, zw size). textureLoad has no derivative, so it may sit here.
        let src_size = textureDimensions(u_texture);
        let box = round(in.carve);
        let dst_size = vec2<u32>(max(box.zw, vec2<f32>(1.0, 1.0)));
        let offset = vec2<i32>(floor(in.position.xy)) - vec2<i32>(box.xy);
        let p = vec2<u32>(clamp(offset, vec2<i32>(0, 0), vec2<i32>(dst_size) - vec2<i32>(1, 1)));
        let s = min(((2u * p + vec2<u32>(1u, 1u)) * src_size) / (2u * dst_size),
                    src_size - vec2<u32>(1u, 1u));
        color = color * textureLoad(u_texture, vec2<i32>(s), 0);
    } else if (in.mode > 0.5 && in.mode < 1.5) {
        let d = sd_round_box(in.local, in.half_size, in.radius);
        var alpha = 0.0;
        if (pc.aa_radius <= 0.0) {
            alpha = select(0.0, 1.0, d <= 0.0);
        } else {
            alpha = 1.0 - smoothstep(-edge_radius, edge_radius, d);
        }
        color.a = color.a * alpha;
    } else if (in.mode > 1.5 && in.mode < 2.5) {
        var texel = sampled;
        texel.a = min(texel.a * (1.0 + (in.mode - 2.0) * 4.0), 1.0);
        color = color * texel;
    } else if (in.mode > 1000.0) {
        // Box shadow: mode is 1024 + sigma (see batched2d.frag); sigma 0 is the hard-edged shadow.
        let sigma = in.mode - 1024.0;
        let shadow_half = in.half_size - vec2<f32>(2.0 * sigma, 2.0 * sigma);
        let d = sd_round_box(in.local, shadow_half, in.radius);
        var cover = 0.0;
        if (sigma > 0.0) {
            cover = 0.5 * (1.0 - erf_approx(d / (sigma * 1.41421356)));
        } else if (pc.aa_radius <= 0.0) {
            cover = select(0.0, 1.0, d <= 0.0);
        } else {
            cover = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        // CSS clips an outer shadow inside the border box (CSS Backgrounds 3 section 7.1.1); the
        // carved box arrives in the vertex stream and its radius is the shadow's minus the inset.
        if (in.carve.z > 0.0 && in.carve.w > 0.0) {
            let inset = in.half_size.x - 2.0 * sigma - in.carve.z;
            let hole_half = in.carve.zw;
            let hole_radius = clamp(in.radius - inset, 0.0, min(hole_half.x, hole_half.y));
            if (sd_round_box(in.local - in.carve.xy, hole_half, hole_radius) <= 0.0) {
                cover = 0.0;
            }
        }
        color.a = color.a * cover;
    } else if (in.mode > 2000.0) {
        // Inset box shadow (issue #612): mode is 2048 + sigma (see batched2d.frag). The quad is the
        // border box and `carve` is the inner box, so the shadow fills the ring between them.
        let sigma = in.mode - 2048.0;
        var inner_half = in.half_size;
        var inner_centre = vec2<f32>(0.0, 0.0);
        if (in.carve.z > 0.0 && in.carve.w > 0.0) {
            inner_half = in.carve.zw;
            inner_centre = in.carve.xy;
        }
        let inset = in.half_size.x - inner_half.x;
        let inner_radius = clamp(in.radius - inset, 0.0, min(inner_half.x, inner_half.y));
        let d = sd_round_box(in.local - inner_centre, inner_half, inner_radius);
        var cover = 0.0;
        if (sigma > 0.0) {
            cover = 0.5 * (1.0 + erf_approx(-d / (sigma * 1.41421356)));
        } else if (pc.aa_radius <= 0.0) {
            cover = select(0.0, 1.0, d <= 0.0);
        } else {
            cover = 1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, d);
        }
        let outer = sd_round_box(in.local, in.half_size, in.radius);
        if (pc.aa_radius <= 0.0) {
            cover = select(0.0, cover, outer <= 0.0);
        } else {
            cover = cover * (1.0 - smoothstep(-pc.aa_radius, pc.aa_radius, outer));
        }
        color.a = color.a * cover;
    } else if (in.mode > 2.5) {
        // Rounded-rect outline: mode is 3 + the stroke width (see batched2d.frag).
        let w = in.mode - 3.0;
        let outer = sd_round_box(in.local, in.half_size, in.radius);
        let inner = sd_round_box(in.local, in.half_size - vec2<f32>(w, w), max(in.radius - w, 0.0));
        let d = max(outer, -inner);
        var alpha = 0.0;
        if (pc.aa_radius <= 0.0) {
            alpha = select(0.0, 1.0, d <= 0.0);
        } else {
            alpha = 1.0 - smoothstep(-edge_radius, edge_radius, d);
        }
        color.a = color.a * alpha;
    }
    color.a = color.a * clip_alpha;
    return color;
}
