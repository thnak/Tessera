#version 450
// SPDX-License-Identifier: MIT

// Storage-buffer conformance probe (RHI 1.12, ADR-068 slice 1). The fragment stage reads one
// read-only storage buffer and writes the value it found, so a test can prove the binding reaches
// the shader: the buffer holds a known colour and the target must come back that colour.
//
// This is a conformance fixture, not a production shader: it exists so `bind_storage_buffer` is
// proven end to end on a real device instead of only structurally on the null backend.

layout(set = 0, binding = 0, std430) readonly buffer ClipRecords {
    vec4 color;
} records;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = records.color;
}
