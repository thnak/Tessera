// SPDX-License-Identifier: MIT
//
// WGSL twin of tests/gpu/rhi/compute_conformance.comp (RHI 1.18/1.22, issue #684): one shader
// selected by a push-constant mode, every result exactly computable on the CPU.
//   mode 0  write indirect-dispatch arguments: out[0..2] = (count, 1, 1)         (1 invocation)
//   mode 1  clear a 16-bin histogram: out[i] = 0 for i < 16
//   mode 2  histogram: atomicAdd(out[in[i] % 16], 1) for i < count
//   mode 3  exclusive prefix sum of in[0..count) into out, one workgroup, shared memory
// A compute program follows the registry contract of `WgslProgram::compute_entry`: storage buffers at
// group 0 bindings 0..n-1, every one `read_write`, and the push-constant block at binding 8.

struct Push {
    mode: u32,
    count: u32,
}

struct OutWords {
    data: array<atomic<u32>>,
}

struct InWords {
    data: array<u32>,
}

@group(0) @binding(0) var<storage, read_write> out_buf: OutWords;
@group(0) @binding(1) var<storage, read_write> in_buf: InWords;
@group(0) @binding(8) var<uniform> pc: Push;

var<workgroup> scan: array<u32, 256>;

@compute @workgroup_size(256)
fn main(@builtin(global_invocation_id) gid: vec3<u32>,
        @builtin(local_invocation_id) lid: vec3<u32>) {
    let i = gid.x;
    if (pc.mode == 0u) {
        if (i == 0u) {
            atomicStore(&out_buf.data[0], pc.count);
            atomicStore(&out_buf.data[1], 1u);
            atomicStore(&out_buf.data[2], 1u);
        }
    } else if (pc.mode == 1u) {
        if (i < 16u) {
            atomicStore(&out_buf.data[i], 0u);
        }
    } else if (pc.mode == 2u) {
        if (i < pc.count) {
            atomicAdd(&out_buf.data[in_buf.data[i] % 16u], 1u);
        }
    } else {
        // Hillis-Steele inclusive scan over one workgroup; the exclusive result is the neighbour.
        let l = lid.x;
        scan[l] = select(0u, in_buf.data[l], l < pc.count);
        workgroupBarrier();
        for (var stride = 1u; stride < 256u; stride = stride << 1u) {
            var add = 0u;
            if (l >= stride) {
                add = scan[l - stride];
            }
            workgroupBarrier();
            scan[l] = scan[l] + add;
            workgroupBarrier();
        }
        if (l < pc.count) {
            atomicStore(&out_buf.data[l], select(scan[l - 1u], 0u, l == 0u));
        }
    }
}
