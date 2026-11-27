// SPDX-License-Identifier: MIT
//
// WGSL twin of src/rendering/backend/vulkan/shaders/path_raster.comp (ADR-071, issue #683): the GPU
// path rasterizer, four passes as four entry points of one module (WGSL has no defines):
//   clear_main   work[0] = 0 (node counter), every row head = 0xFFFFFFFF.
//   stroke_main  one thread per stroke segment: expands it into edges written after the fill segments
//                (a quad for the body, a wedge for a round join, a half disc for a round cap).
//   bin_main     one thread per segment: for each pixel row it crosses, push a node {segment, next}
//                onto that (path, row) list (atomic counter + atomic exchange).
//   fine_main    one thread per pixel: exact signed area of every segment on the (path, row) list,
//                fill rule, source-over into the straight-alpha RGBA8 output.
// The buffer layouts are src/rendering/path/gpu_path_encoder.hpp, shared with the GLSL source; the
// bindings are the 'path_raster' entry of tools/shaders/manifest.json. Differences from the GLSL that
// are not spelling: the work buffer is array<atomic<u32>> (WGSL cannot mix atomic and plain access
// to one array, so every access goes through atomicLoad/atomicStore, which is relaxed and enough
// because each pass is its own dispatch), GLSL's mod() is written out (WGSL's % truncates), and the
// push constant is a uniform (WebGPU has none). A batch of N rasters is N job records and the job is
// the workgroup id's y (the fine pass: z), as in the GLSL.
//
// Run by the WebGPU backend (RHI 1.22, docs/design/gpu/compute.md section 3.1): the file is embedded
// and registered as four programs (one per entry point, ids in webgpu_path_raster.hpp). The compute
// contract fixes two things that differ from the GLSL's interface: every storage buffer at group 0,
// bindings 0..6, is declared read_write (WebGPU rejects a layout whose access disagrees, and the RHI
// binds one kind of buffer), and the push block is the uniform at binding 8 (kBindingComputePush),
// not 7. scripts/ci/validate_wgsl.py validates it with naga.

@group(0) @binding(0) var<storage, read_write> segs: array<u32>;
@group(0) @binding(1) var<storage, read_write> paths: array<u32>;
@group(0) @binding(2) var<storage, read_write> work: array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> pixels: array<u32>;
@group(0) @binding(4) var<storage, read_write> strokes: array<u32>;
@group(0) @binding(5) var<storage, read_write> dashes: array<u32>;
@group(0) @binding(6) var<storage, read_write> jobs: array<u32>;

struct Push {
    job_base: u32,
}
@group(0) @binding(8) var<uniform> pcb: Push;

struct Job {
    width: u32,
    height: u32,
    path_count: u32,
    seg_count: u32,
    node_capacity: u32,
    nodes_base: u32,
    fill_count: u32,
    exp_base: u32,
    stroke_count: u32,
    arc_edges: u32,
    out_stride: u32,
    spare: u32,
}

// The raster this invocation works on, and where its slices of the shared buffers start (words).
var<private> pc: Job;
var<private> sb: u32;
var<private> pb: u32;
var<private> wb: u32;
var<private> ob: u32;
var<private> kb: u32;
var<private> db: u32;

const kNil: u32 = 0xFFFFFFFFu;
const kPi: f32 = 3.14159265358979;

fn load_job(index: u32) {
    let b = (pcb.job_base + index) * 20u;
    pc = Job(jobs[b], jobs[b + 1u], jobs[b + 2u], jobs[b + 3u], jobs[b + 4u], jobs[b + 5u],
             jobs[b + 6u], jobs[b + 7u], jobs[b + 8u], jobs[b + 9u], jobs[b + 10u], jobs[b + 11u]);
    sb = jobs[b + 12u];
    pb = jobs[b + 13u];
    wb = jobs[b + 14u];
    ob = jobs[b + 15u];
    kb = jobs[b + 16u];
    db = jobs[b + 17u];
}

fn wload(i: u32) -> u32 {
    return atomicLoad(&work[wb + i]);
}

fn wstore(i: u32, v: u32) {
    atomicStore(&work[wb + i], v);
}

fn f32_at_strokes(i: u32) -> f32 {
    return bitcast<f32>(strokes[kb + i]);
}

fn f32_at_dashes(i: u32) -> f32 {
    return bitcast<f32>(dashes[db + i]);
}

// GLSL mod(): x - y * floor(x / y), never negative for a positive y.
fn glsl_mod(x: f32, y: f32) -> f32 {
    return x - y * floor(x / y);
}

// A path's transform: x' = a x + c y + e, y' = b x + d y + f.
struct Xf {
    abcd: vec4<f32>,
    ef: vec2<f32>,
}

fn path_xf(path: u32) -> Xf {
    let b = pb + path * 16u;
    var m: Xf;
    m.abcd = vec4<f32>(bitcast<f32>(paths[b]), bitcast<f32>(paths[b + 1u]),
                       bitcast<f32>(paths[b + 2u]), bitcast<f32>(paths[b + 3u]));
    m.ef = vec2<f32>(bitcast<f32>(paths[b + 4u]), bitcast<f32>(paths[b + 5u]));
    return m;
}

fn xf_apply(m: Xf, p: vec2<f32>) -> vec2<f32> {
    return vec2<f32>(m.abcd.x * p.x + m.abcd.z * p.y + m.ef.x,
                     m.abcd.y * p.x + m.abcd.w * p.y + m.ef.y);
}

// A segment id below fill_count is a fill edge from the CPU; the rest were expanded from strokes.
fn seg_word(s: u32, k: u32) -> u32 {
    if (s < pc.fill_count) {
        return segs[sb + s * 8u + k];
    }
    return wload(pc.exp_base + (s - pc.fill_count) * 8u + k);
}

// ---- clear -----------------------------------------------------------------------------------

@compute @workgroup_size(256)
fn clear_main(@builtin(workgroup_id) wid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>) {
    load_job(wid.y);
    let i = gid.x;
    let total = 1u + pc.path_count * pc.height;
    if (i == 0u) {
        wstore(0u, 0u);
    } else if (i < total) {
        wstore(i, kNil);
    }
}

// ---- stroke ----------------------------------------------------------------------------------

fn put_edge(slot: u32, a: vec2<f32>, b: vec2<f32>, path: u32) {
    let o = pc.exp_base + slot * 8u;
    wstore(o, bitcast<u32>(a.x));
    wstore(o + 1u, bitcast<u32>(a.y));
    wstore(o + 2u, bitcast<u32>(b.x));
    wstore(o + 3u, bitcast<u32>(b.y));
    wstore(o + 4u, path);
}

// Zero-length edges: the bin pass skips them, so an unused slot costs one thread and nothing else.
fn put_zero(slot: u32, count: u32, path: u32) {
    for (var k = 0u; k < count; k++) {
        put_edge(slot + k, vec2<f32>(0.0), vec2<f32>(0.0), path);
    }
}

// Apex p, first rim point p + a, then the rim arc turning counter-clockwise by sweep, then back to p.
// sweep <= 0 writes nothing (zero-length edges are skipped by the bin pass).
fn put_wedge(slot: u32, p: vec2<f32>, a: vec2<f32>, sweep: f32, path: u32) {
    let arc = pc.arc_edges;
    // The rim needs one edge per step of the full-circle tessellation (a half turn is `arc` edges),
    // so a join that barely turns writes one edge instead of the whole fan; the rest of the slots stay
    // zero-length and cost the bin pass a thread each, nothing else.
    var used = 0u;
    if (sweep > 0.0) {
        used = clamp(u32(ceil(sweep / kPi * f32(arc))), 1u, arc);
    }
    if (used == 0u) {
        put_zero(slot, arc + 2u, path);
        return;
    }
    // Each rim point is rotated from a by its own angle rather than stepped, so no error accumulates.
    var prev = a;
    put_edge(slot, p, p + a, path);
    for (var k = 0u; k < used; k++) {
        let angle = sweep * f32(k + 1u) / f32(used);
        let c = cos(angle);
        let s = sin(angle);
        let next = vec2<f32>(c * a.x - s * a.y, s * a.x + c * a.y);
        put_edge(slot + 1u + k, p + prev, p + next, path);
        prev = next;
    }
    put_edge(slot + 1u + used, p + prev, p, path);
    put_zero(slot + used + 2u, arc + 2u - (used + 2u), path);
}

// A bevel (tip_valid false) or miter join: apex p, the two outer offsets a and b, the miter tip when
// valid. Wound like the wedges, so the edges p->a and b->p cancel against the neighbouring bodies.
fn put_corner(slot: u32, p: vec2<f32>, a: vec2<f32>, b: vec2<f32>, tip: vec2<f32>, tip_valid: bool,
              path: u32) {
    let size = pc.arc_edges + 2u;
    put_edge(slot, p, p + a, path);
    if (tip_valid) {
        put_edge(slot + 1u, p + a, p + tip, path);
        put_edge(slot + 2u, p + tip, p + b, path);
        put_edge(slot + 3u, p + b, p, path);
        put_zero(slot + 4u, size - 4u, path);
    } else {
        put_edge(slot + 1u, p + a, p + b, path);
        put_edge(slot + 2u, p + b, p, path);
        put_zero(slot + 3u, size - 3u, path);
    }
}

// A square cap: the body's end edge reversed (so it cancels), then out by hw along extend.
// side is the offset to the first corner (p + side first), extend is the extension.
fn put_square(slot: u32, p: vec2<f32>, side: vec2<f32>, extend: vec2<f32>, path: u32) {
    put_edge(slot, p + side, p - side, path);
    put_edge(slot + 1u, p - side, p - side + extend, path);
    put_edge(slot + 2u, p - side + extend, p + side + extend, path);
    put_edge(slot + 3u, p + side + extend, p + side, path);
    put_zero(slot + 4u, pc.arc_edges + 2u - 4u, path);
}

// One run of a stroke along a segment direction d1: the body from pa to pb, then the start (a join
// against the previous vertex, or a cap) and the end (a cap, when this run ends the stroke or a dash).
// Occupies 7 + 2 * (arc_edges + 2) edge slots from slot: 7 for the body, the rest for the wedges. The
// inside-of-a-turn trimming is explained in the GLSL source.
fn emit_piece(slot: u32, pa: vec2<f32>, pb: vec2<f32>, d1: vec2<f32>, hw: f32, join_start: bool,
              end_cap: bool, prev_point: vec2<f32>, back_run: f32, join_kind: u32,
              miter_limit: f32, cap_kind: u32, path: u32) {
    let wedge = pc.arc_edges + 2u;
    let n1 = vec2<f32>(-d1.y, d1.x) * hw;
    var sweep = 0.0;
    var a = n1;
    var b = n1;
    var trimmed = false;
    if (join_start) {
        let dv = pa - prev_point;
        let dl = length(dv);
        if (dl > 0.0) {
            let d0 = dv / dl;
            let cr = d0.x * d1.y - d0.y * d1.x;
            sweep = atan2(abs(cr), dot(d0, d1));
            let n0 = vec2<f32>(-d0.y, d0.x) * hw;
            if (cr > 0.0) {
                a = -n0;
                b = -n1;
            } else {
                a = n1;
                b = n0;
            }
            let t = hw * tan(sweep * 0.5);
            if (sweep >= 1e-4 && sweep < 3.0 && t <= min(dl, back_run) && t <= length(pb - pa)) {
                trimmed = true;
                let inner = (n0 + n1) / (1.0 + cos(sweep));
                if (cr > 0.0) {
                    let corner = pa + inner;
                    put_edge(slot, pa - n1, pb - n1, path);
                    put_edge(slot + 1u, pb - n1, pb + n1, path);
                    put_edge(slot + 2u, pb + n1, corner, path);
                    put_edge(slot + 3u, corner, pa + n0, path);
                    put_edge(slot + 4u, pa + n0, pa, path);
                    put_edge(slot + 5u, pa, pa - n1, path);
                } else {
                    let corner = pa - inner;
                    put_edge(slot, pa + n1, pa, path);
                    put_edge(slot + 1u, pa, pa - n0, path);
                    put_edge(slot + 2u, pa - n0, corner, path);
                    put_edge(slot + 3u, corner, pb - n1, path);
                    put_edge(slot + 4u, pb - n1, pb + n1, path);
                    put_edge(slot + 5u, pb + n1, pa + n1, path);
                }
                put_zero(slot + 6u, 1u, path);
            }
        }
    }
    if (!trimmed) {
        // body: wound so successive edges turn the same way as the wedges below
        put_edge(slot, pa + n1, pa - n1, path);
        put_edge(slot + 1u, pa - n1, pb - n1, path);
        put_edge(slot + 2u, pb - n1, pb + n1, path);
        put_edge(slot + 3u, pb + n1, pa + n1, path);
        put_zero(slot + 4u, 3u, path);
    }
    let start_slot = slot + 7u;
    if (join_start) {
        if (sweep < 1e-4) {
            put_zero(start_slot, wedge, path);
        } else if (join_kind == 0u) {
            put_wedge(start_slot, pa, a, sweep, path);
        } else {
            // 1 / cos(sweep / 2) is the miter length over the stroke width: past the limit (or a
            // reversal, where it is unbounded) the join falls back to a bevel, as SVG specifies.
            let miter = join_kind == 2u && cos(sweep * 0.5) * miter_limit >= 1.0;
            let tip = (a + b) / (1.0 + cos(sweep));
            put_corner(start_slot, pa, a, b, tip, miter, path);
        }
    } else if (cap_kind == 0u) {
        put_wedge(start_slot, pa, n1, kPi, path);
    } else if (cap_kind == 2u) {
        put_square(start_slot, pa, -n1, -d1 * hw, path);
    } else {
        put_zero(start_slot, wedge, path);
    }
    let end_slot = start_slot + wedge;
    if (!end_cap || cap_kind == 1u) {
        put_zero(end_slot, wedge, path);
    } else if (cap_kind == 0u) {
        put_wedge(end_slot, pb, -n1, kPi, path);
    } else {
        put_square(end_slot, pb, n1, d1 * hw, path);
    }
}

@compute @workgroup_size(256)
fn stroke_main(@builtin(workgroup_id) wid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>) {
    load_job(wid.y);
    let i = gid.x;
    if (i >= pc.stroke_count) {
        return;
    }
    let r = i * 16u;
    let p0 = vec2<f32>(f32_at_strokes(r), f32_at_strokes(r + 1u));
    let p1 = vec2<f32>(f32_at_strokes(r + 2u), f32_at_strokes(r + 3u));
    let path = strokes[kb + r + 4u];
    let hw = f32_at_strokes(r + 5u);
    let flags = strokes[kb + r + 6u]; // 1 end of an open contour, 2 has a previous vertex, 4 contour start, 8 contour end
    let join_kind = strokes[kb + r + 7u]; // 0 round, 1 bevel, 2 miter
    let prev_point = vec2<f32>(f32_at_strokes(r + 8u), f32_at_strokes(r + 9u));
    let miter_limit = f32_at_strokes(r + 10u);
    let cap_kind = strokes[kb + r + 11u]; // 0 round, 1 butt, 2 square
    let s0 = f32_at_strokes(r + 12u);     // path length up to p0
    let slot = strokes[kb + r + 13u];     // first edge slot of this record
    let pieces = strokes[kb + r + 14u];   // dash pieces this record has room for (1 = solid)
    let wedge = pc.arc_edges + 2u;
    let stride = 7u + 2u * wedge;
    let d = p1 - p0;
    let len = length(d);
    if (len <= 0.0) {
        put_zero(slot, stride * pieces, path);
        return;
    }
    let d1 = d / len;
    let dbase = path * 12u;
    let dn = dashes[db + dbase]; // dash entries (alternating on / off), 0 = solid
    if (dn == 0u) {
        emit_piece(slot, p0, p1, d1, hw, (flags & 2u) != 0u, (flags & 1u) != 0u, prev_point, 1e30,
                   join_kind, miter_limit, cap_kind, path);
        return;
    }
    // Dashes: walk the pattern across [s0, s0 + len]. The pattern phase is (s + offset) mod period, so
    // an animated offset is one float per path; the CPU split every segment to at most one period, so
    // pieces (on-entries + 1) always covers what a segment can hold.
    let offset = f32_at_dashes(dbase + 1u);
    let period = f32_at_dashes(dbase + 2u);
    let s1 = s0 + len;
    var ph = glsl_mod(s0 + offset, period);
    var k = 0u;
    var c0 = 0.0;
    for (; k < dn; k++) {
        let v = f32_at_dashes(dbase + 4u + k);
        if (ph < c0 + v) {
            break;
        }
        c0 += v;
    }
    if (k >= dn) {
        k = 0u;
        c0 = 0.0;
        ph = 0.0;
    }
    var q = s0;
    var used = 0u;
    for (var guard = 0u; guard < 96u && q < s1 - 1e-4 && used < pieces; guard++) {
        let v = f32_at_dashes(dbase + 4u + k);
        let rem = c0 + v - ph;
        let qe = min(q + rem, s1);
        if (rem > 1e-3 && (k & 1u) == 0u) {
            let starts_here = ph - c0 < 1e-3;
            let at_contour_start = (flags & 4u) != 0u && q <= s0;
            let join_start = !starts_here && !at_contour_start && (flags & 2u) != 0u;
            // A dash that ends inside (or exactly at the end of) this segment is capped; one that runs
            // on into the next segment is left for that segment's record to join.
            let end_cap = q + rem <= s1 + 1e-3 || ((flags & 8u) != 0u && qe >= s1 - 1e-3);
            emit_piece(slot + used * stride, p0 + d1 * (q - s0), p0 + d1 * (qe - s0), d1, hw,
                       join_start, end_cap, prev_point, ph - c0, join_kind, miter_limit, cap_kind,
                       path);
            used++;
        }
        ph += qe - q;
        q = qe;
        if (ph >= c0 + v - 1e-3) {
            ph = c0 + v;
            c0 = ph;
            k++;
            if (k >= dn) {
                k = 0u;
                c0 = 0.0;
                ph = 0.0;
            }
        }
    }
    put_zero(slot + used * stride, (pieces - used) * stride, path);
}

// ---- bin -------------------------------------------------------------------------------------

@compute @workgroup_size(256)
fn bin_main(@builtin(workgroup_id) wid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>) {
    load_job(wid.y);
    let i = gid.x;
    if (i >= pc.seg_count) {
        return;
    }
    let path = seg_word(i, 4u);
    let m = path_xf(path);
    let y0 = xf_apply(m, vec2<f32>(bitcast<f32>(seg_word(i, 0u)), bitcast<f32>(seg_word(i, 1u)))).y;
    let y1 = xf_apply(m, vec2<f32>(bitcast<f32>(seg_word(i, 2u)), bitcast<f32>(seg_word(i, 3u)))).y;
    if (y0 == y1) {
        return;
    }
    let ymin = min(y0, y1);
    let ymax = max(y0, y1);
    let row0 = max(i32(floor(ymin)), 0);
    let row1 = min(i32(ceil(ymax)) - 1, i32(pc.height) - 1);
    for (var row = row0; row <= row1; row++) {
        let node = atomicAdd(&work[wb], 1u);
        if (node < pc.node_capacity) {
            let head_index = 1u + path * pc.height + u32(row);
            let previous = atomicExchange(&work[wb + head_index], node);
            wstore(pc.nodes_base + node * 2u, i);
            wstore(pc.nodes_base + node * 2u + 1u, previous);
        }
    }
}

// ---- fine ------------------------------------------------------------------------------------

// Signed area of the pixel [px, px+1] x [py, py+1] lying to the right of segment (a_in, b_in).
fn segment_area(a_in: vec2<f32>, b_in: vec2<f32>, px: f32, py: f32) -> f32 {
    var a = a_in;
    var b = b_in;
    var sign_v = 1.0;
    if (a.y > b.y) {
        let t = a;
        a = b;
        b = t;
        sign_v = -1.0;
    }
    let y0 = max(a.y, py);
    let y1 = min(b.y, py + 1.0);
    if (y1 <= y0) {
        return 0.0;
    }
    let inv_slope = (b.x - a.x) / (b.y - a.y);
    let xa = a.x + (y0 - a.y) * inv_slope;
    let xb = a.x + (y1 - a.y) * inv_slope;
    let dy = y1 - y0;
    if (max(xa, xb) <= px) {
        return sign_v * dy;
    }
    if (min(xa, xb) >= px + 1.0) {
        return 0.0;
    }
    let dx = xb - xa;
    var ts: array<f32, 4>;
    var n = 0;
    ts[n] = 0.0;
    n++;
    if (dx != 0.0) {
        let tl = (px - xa) / dx;
        let tr = (px + 1.0 - xa) / dx;
        let lo = min(tl, tr);
        let hi = max(tl, tr);
        if (lo > 0.0 && lo < 1.0) {
            ts[n] = lo;
            n++;
        }
        if (hi > 0.0 && hi < 1.0) {
            ts[n] = hi;
            n++;
        }
    }
    ts[n] = 1.0;
    n++;
    var area = 0.0;
    for (var k = 0; k + 1 < n; k++) {
        let xm = xa + dx * (ts[k] + ts[k + 1]) * 0.5;
        area += dy * (ts[k + 1] - ts[k]) * (px + 1.0 - clamp(xm, px, px + 1.0));
    }
    return sign_v * area;
}

@compute @workgroup_size(16, 16)
fn fine_main(@builtin(workgroup_id) wid: vec3<u32>, @builtin(global_invocation_id) gid: vec3<u32>) {
    load_job(wid.z);
    let x = gid.x;
    let y = gid.y;
    if (x >= pc.width || y >= pc.height) {
        return;
    }
    let px = f32(x);
    let py = f32(y);
    var dst = vec4<f32>(0.0);
    for (var p = 0u; p < pc.path_count; p++) {
        let base = pb + p * 16u;
        if (i32(x) < i32(paths[base + 8u]) || i32(x) >= i32(paths[base + 10u]) ||
            i32(y) < i32(paths[base + 9u]) || i32(y) >= i32(paths[base + 11u])) {
            continue;
        }
        let m = path_xf(p);
        var w = 0.0;
        var node = wload(1u + p * pc.height + y);
        while (node != kNil) {
            let s = wload(pc.nodes_base + node * 2u);
            let a = xf_apply(m, vec2<f32>(bitcast<f32>(seg_word(s, 0u)), bitcast<f32>(seg_word(s, 1u))));
            let b = xf_apply(m, vec2<f32>(bitcast<f32>(seg_word(s, 2u)), bitcast<f32>(seg_word(s, 3u))));
            w += segment_area(a, b, px, py);
            node = wload(pc.nodes_base + node * 2u + 1u);
        }
        let t = abs(w);
        var cov: f32;
        if (paths[base + 6u] == 0u) {
            cov = min(t, 1.0);
        } else {
            let f = glsl_mod(t, 2.0);
            cov = select(f, 2.0 - f, f > 1.0);
        }
        if (cov <= 0.0) {
            continue;
        }
        let rgba = paths[base + 7u];
        let c = vec4<f32>(f32(rgba & 255u), f32((rgba >> 8u) & 255u), f32((rgba >> 16u) & 255u),
                          f32(rgba >> 24u)) / 255.0;
        let src = vec4<f32>(c.rgb * c.a, c.a) * cov;
        dst = src + dst * (1.0 - src.a);
    }
    var rgb = vec3<f32>(0.0);
    if (dst.a > 0.0) {
        rgb = dst.rgb / dst.a;
    }
    let q = vec4<u32>(clamp(vec4<f32>(rgb, dst.a) * 255.0 + 0.5, vec4<f32>(0.0), vec4<f32>(255.0)));
    pixels[ob + y * pc.out_stride + x] = q.r | (q.g << 8u) | (q.b << 16u) | (q.a << 24u);
}
