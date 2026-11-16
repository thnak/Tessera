Closes **#199**. Rebased on `origin/main` **18f66b06**. Acceptance: release builds on all lanes run validation-layer and golden suites and publish artifacts.

### What the existing lanes already covered — and what they did not

`release-rehearsal` and `static-runtime-proof` both cover the **artifact** half — CPack, package contents, static linking, reproducibility, SBOM, license bundle — and **neither runs `ctest` or touches a GPU path**. So the runtime half of this acceptance was genuinely uncovered. This adds it to the release build jobs rather than creating a new workflow, because a new workflow would duplicate a full Release build on every push, against the CI-load rule.

### The finding that justifies the whole change

`batched2d_vulkan_validation` prints `[vulkan] warning: validation layer not available` and then **passes**. So a Linux lane without the validation layer was **green having validated nothing at all**. The lane now asserts that text is absent, and the same run fails with:

```
FAIL: the output contains forbidden text: 'validation layer not available'
```

Verified by feeding the gate a log containing that line. A suite that reports success while doing nothing is worse than no suite, because it is **evidence-shaped**.

### No new dependency

Validation layers and lavapipe were **already** installed by `linux.yml` and already recorded in `docs/specs/dependencies.md` ("Vulkan ValidationLayers … dev / CI", "Mesa lavapipe"). The layer is selected at **runtime** by `rhi::DeviceDesc::validation`, not by a build option — there is no `TESSERA_VULKAN_VALIDATION` and none was added, so a Release build runs it identically.

### "All lanes" cannot be literally true, so the matrix is explicit instead

Windows cannot run lavapipe goldens (no CPU Vulkan ICD on a hosted runner), cannot build the Vulkan backend at all (`TESSERA_BUILD_VULKAN=OFF`), and the ThorVG-gated suites need a FetchContent network fetch in a job already documented as time-tight. Rather than claim more than is true, `scripts/check_release_suites.py` encodes a **`LANE_MATRIX`**:

```
note: linux: 15 required, 0 declared unable to run
note: windows: 6 required, 10 declared unable to run
release suite matrix ok: 2 lane(s)
```

Those 10 windows suites are **declared with reasons**, so they are visible rather than silently absent. Two modes:

- **`--workflows`** asserts each lane's suite step declares exactly the matrix set. This catches "the lane quietly dropped a suite" — a runtime report *cannot*, because a suite that is never declared never appears in the JUnit file.
- **`--lane`** reads the `--output-junit` report plus the run log, and asserts every required suite exists and **ran rather than skipped**, plus the forbidden-text check above.

| suite group | linux | windows | why not |
|---|---|---|---|
| `batched2d_vulkan_validation`, `vulkan_*`, `glass_*_gpu`, `msaa`, `rounded_clip`, `paint_*_gpu` | run | **cannot** | `TESSERA_BUILD_VULKAN=OFF` |
| `batched2d_golden/_overlay/_text/_shapes`, `gallery_golden_*`, `harness_frame_capture` | run | **cannot** | registered only under `UNIX`; no lavapipe on Windows |
| `batched2d_cpu_image_golden`, `cpu_wide_golden`, `theme_golden`, `harness_golden`, `lowering_golden`, `schema_golden` | run | **run** | GPU-free, tolerance 0, bit-exact both hosts |
| `batched2d_cpu_golden`, `batched2d_cpu_text_golden` | run | **cannot** | need `batched2d_poc` (links the Vulkan RHI) |
| `batched2d_cpu_vector_golden`, `codegen_vector_paint_golden` | run | **cannot** | `TESSERA_ENABLE_THORVG` network fetch |

### What the first CI run caught — and the fix

The linux lane failed on its first push, and it was **my** bug, not the gate's: the workflow passed `--require X --forbid-text Y` on the command line, but the gate reads both from `LANE_MATRIX` keyed by `--lane`. My own design was right — the **script** should own the list, or `--workflows` and the runtime step can disagree — so I fixed the workflows, not the script:

`check_release_suites.py: error: unrecognized arguments: --require batched2d_vulkan_validation …`

Both lanes now pass **only** `--lane`/`--junit`/`--log`. That removes 20 duplicated flag lines from `windows.yml` and 6 from `linux.yml`, and there is now exactly one list. I also corrected the comment that claimed the build jobs owned those lists, which is no longer true.

This is worth stating plainly: a workflow change cannot be tested locally, so the first CI run **is** the test — and it found the mismatch immediately rather than after a green-looking pass.

### Two more real bugs, found by testing against actual output

1. `ctest --preset` resolves a **relative** `--output-junit` against the preset's test directory, writing `build/ci/build/ci/…`. Both workflows now pass an **absolute** path.
2. Windows `Tee-Object` logs are **UTF-16LE**, which the gate now detects rather than failing to parse.

### Artifacts

`release-artifacts-linux` and `release-artifacts-windows` (30 days): every golden PNG (frame, diff, candidate baseline), the JUnit report, the CTest log. The **archive is deliberately not uploaded** — `packaging.md` §5 keeps it a release-time artifact. No GitHub Release, no tag.

### Files

**Modified:** `.github/workflows/linux.yml` (JUnit capture, `pipefail`, gate step, artifact upload, matrix check in `release-gates`), `.github/workflows/windows.yml` (same pattern), `docs/guides/packaging.md` (new "Release CI lanes" section with the matrix and rationale — 401 lines, under the 600 hard limit).
**Added:** `scripts/check_release_suites.py` (368), `scripts/tests/test_check_release_suites.py` (366, 18 fixture tests).

No new doc was added, because `docs/README.md` is off-limits and an unindexed doc fails the docs gate.

### Verification

```
yaml ok                                                   # all workflows parse
python -B scripts/check_release_suites.py --workflows     -> matrix ok: 2 lanes, 0 flag duplication
unittest test_check_release_suites.py                     -> 18 tests OK
cmake --build --preset vs2026-release --parallel 12       -> ok
ctest --preset vs2026-release --parallel 12               -> 594 tests, 100% passed
unittest discover -s scripts/tests                        -> 279 tests OK
python -B scripts/check_source_size.py                    -> 0 errors
python -B scripts/gates.py                                -> 7 of 8 pass
```

The single `gates.py` failure is `format-version` reporting `clang-format not found` — it also fails on pristine main on this machine (exit 2).

The gate was verified to **fail** in four ways: a dropped suite, an `--allow-skip` without a reason, an unknown suite name, and the layer warning present. It also reads a real 595-case Windows JUnit report correctly.

### What is only provable on the next CI run — stated, not assumed

The `-R` filter selecting these suites on a real checkout; that `vulkan-validationlayers` installs on today's `ubuntu-latest` (assertions fail closed if not); the bash `tee`/`pipefail` and pwsh `Tee-Object` plumbing end to end; and the artifact upload paths and sizes.

### Declined rather than silently skipped

- **ThorVG on Windows** — network fetch plus 37 sources into a job already time-tight.
- **WebGPU pixels** — the owner decision the preview lane is already waiting on (`compiler-wasm.md` §8).

Closes #199
