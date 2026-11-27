# ADR-076: Per-fragment gradients, RHI 1.20

- Status: **Accepted** (owner-approved big slice, 2026-11-27)
- Date: 2026-11-27
- Amends: [ADR-068](ADR-068-clip-state-in-a-storage-buffer.md) (the frame record buffer grows a gradient area)
- Related: [ADR-049](ADR-049-backend-extensibility.md) (RHI versioning), issue #617,
  [gradients.md](../docs/design/gpu/gradients.md), [rhi.md](../docs/design/gpu/rhi.md)

## Context

Gradients were emitted as many solid quads (64, 812 or 4096 per layer). They were inexact for oblique
and repeating ramps, left radial corners unpainted, cost up to 1.5 MB of vertices per conic and gave
up to 255/255 error against Chromium.

## Decision

One quad per gradient layer. Stops and parameters go in the existing per-frame storage buffer (a
gradient area after the clip pool); the fragment shader evaluates linear, radial, conic and repeating
gradients exactly (32 stops, 512 gradients, 2048 stops per frame). Interpolation is encoded-space
sRGB, premultiplied as in CSS Images 3.5.3. A non-pure `Device::supports_analytic_gradients()`
(RHI 1.20) gates it; without it, or when the table is full, the sink emits the old bands. The CPU
sink evaluates the same math per pixel.

## Consequences

- Vertex bytes drop from 24 KB to 1.5 MB per layer down to 384 B; batching is unchanged.
- The frame buffer grows by 57344 bytes per frame in flight.
- Translucent mixed-hue ramps change (premultiplied); opaque and same-hue ramps do not.
- Radial gradients now fill the whole box.
- A `repeating-*` ramp is not expanded into stops: record flag bit 0 makes the shader wrap t over the
  ramp's own period (last stop minus first), so any period costs the authored stops only. Stop
  offsets are no longer clamped to 0..1, and a stop position may be a length or, for two positions,
  a pair (gradients.md 3.1).
- Metal stays banded until a device test exists.

## Alternatives

- Ramp texture per gradient: a descriptor per layer, breaks batching.
- Stops in vertices: 4x32 stops per quad.
- Per-draw uniforms: splits batches.
- Keep straight interpolation: simpler, but 64/255 off Chromium.

## Follow-up

Enable Metal after a device test; runtime-test the WGSL path; retire the banded fallback when every
backend reports the capability.
