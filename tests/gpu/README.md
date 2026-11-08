# GPU tests

GPU tests are grouped by RHI responsibility, separate from UI/compiler unit tests. Design
contract: [`docs/design/gpu/testing.md`](../../docs/design/gpu/testing.md).

## Tree

```text
tests/
  gpu/
    rhi/          device/command contract, null stats, validation
    resources/    buffers, textures, targets, caches, lifetime
    pipelines/    cache keys, vertex layout, shader manifest/reflection
    frames/       slots, in-flight retirement, uploads/readbacks, resize
    backends/     capability/parity/validation
    shaders/      shader smoke helpers
    batched2d/    B1 batching/command tests (moved from tests/unit)
  integration/gpu/   end-to-end paths
  golden/gpu/        deterministic baselines
  fixtures/gpu/      committed shaders/images/limits
  benchmarks/gpu/    frame/upload/alloc benchmarks
```

## Rules

- New GPU tests go in `tests/gpu/<area>/`; do not add them to `tests/unit/`.
- Register executables in `tests/gpu/CMakeLists.txt`.
- Null backend for structure; lavapipe/software for pixels; real GPU only for smoke/tolerance.
- No wall clock, sleeps, network, system fonts, random ids, or real-GPU baselines.
- Keep unit tests small; move long fixtures to `tests/fixtures/gpu/`.
- Every bug fix gets a regression test.

## Run

```bash
ctest --preset dev -R 'rhi_|batched2d|gpu'
```

Linux CI uses lavapipe:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json ctest --preset ci
```
