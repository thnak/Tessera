# Compiler in Wasm (design-time preview, V1-M4 P0)

Design note for the Emscripten build of the compiler and the native-versus-Wasm parity gate
(issue #164). Normative context: [ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md)
(self-contained Wasm designer engine), [ADR-049](../../decisions/ADR-049-backend-extensibility.md)
(the WebGPU/Wasm toolchain pin), [`compiler.md`](compiler.md) (the pipeline itself) and
[`reference-equantic-ui.md`](reference-equantic-ui.md) section 7.4 (the P0-P7 staging plan).

## 1. What P0 has to prove

The design-time preview compiles the user's `.tsx` in the browser instead of asking a server for
generated C++. That only works if the engine compiled to Wasm produces the *same compiler output*
as the native compiler: the preview and the native app must agree on the IR, the node identity and
the generated C++, or a preview would be a different program from the build.

P0's acceptance is therefore narrow and total: **the Wasm-built compiler produces IR byte-identical
to the native compiler for `samples/parity-demo/Counter.tsx`**. Byte identity, not "equivalent
output", because the IR JSON is a canonical serialization
([`ir-v1.md`](../specs/ir-v1.md)) — any difference is a real semantic drift, and a digest comparison
makes that drift a one-line failure instead of a review.

## 2. How the compiler becomes a Wasm tool

The compiler already had one important property that made P0 engineering rather than a port: it is
plain portable C++23 whose only inputs are files (`tessera_codegen --emit-ir FILE FILE.tsx...`) and
whose only outputs are files. Nothing in the front end, the analyzer, the lowering, the IR writer or
the code generator touches an OS API the browser can disagree with — it all goes through
`<filesystem>` and `<fstream>`.

So the Wasm build reuses the **same target** rather than a fork:

- The `wasm-webgpu` **configure** preset (Emscripten toolchain, Release, tests/PoC/Vulkan off,
  `-Werror`, `-fwasm-exceptions`) already sets up a cross build of the whole tree. No new configure
  preset is needed.
- A dedicated **build** preset, `wasm-compiler`, selects only `tessera_codegen_cli`, so the native
  WebGPU lane's target set is untouched and the compiler is not silently added to it.
- The only new source change is two linker options on that target, gated on `EMSCRIPTEN`
  (`src/compiler/codegen/CMakeLists.txt`):
  - `-sENVIRONMENT=node` — the artifact is a Node tool, not a browser module.
  - `-sNODERAWFS=1` — routes `<cstdio>`/`<filesystem>` to Node's synchronous `fs` instead of
    Emscripten's in-memory `MEMFS`.

### 2.1 Why `-sNODERAWFS=1` is required, and where it must never appear

With the default `MEMFS`, the Wasm process sees a virtual filesystem that starts empty. The CLI then
fails before it can lower anything:

```text
tessera_codegen: cannot read `samples/parity-demo/Counter.tsx`
```

Node's own `fs` reads the same path fine; the sandbox is the Wasm heap, not the host. `NODERAWFS`
makes the Emscripten runtime forward file operations to the host through Node, so the tool behaves
like the native binary: paths on the command line mean host paths.

`NODERAWFS` is **only** valid for a Node artifact. A browser has no synchronous host filesystem, so
the option is meaningless there and would break the module; it must never be added globally, and it
must never apply to a sample app or any browser-shipped artifact. That is why it lives on this one
target as a `target_link_options` under `if (EMSCRIPTEN)`, not as a directory or project-wide option.

## 3. The native-versus-Wasm parity gate

`scripts/ci/wasm_ir_parity.py` (Python, MIT header, stdlib only, no network) is the executable form
of P0's acceptance. It is a pure comparator: it builds nothing and downloads nothing.

```text
python -B scripts/ci/wasm_ir_parity.py \
    --native build/native-ci/src/compiler/codegen/tessera_codegen \
    --wasm   build/wasm-webgpu/src/compiler/codegen/tessera_codegen.js \
    --node   "$EMSDK_NODE"
```

For each sample it runs both compilers and compares:

1. **IR** (`--emit-ir`): the canonical IR JSON, size and SHA-256, required to match exactly. This is
   the acceptance artifact.
2. **Generated C++** (`--write`), unless `--ir-only` is passed: `Counter.g.hpp`,
   `Counter.hpp`, `Counter.cpp` and `automation.manifest.json`, each byte-compared and the file
   *sets* compared too (a silently dropped artifact fails). The generated C++ is what the preview
   pipeline consumes downstream, so comparing it extends the gate from the front end to codegen;
   it was verified byte-identical when the gate was written, so the assertion is drift coverage
   rather than an aspiration.

On success it prints one line per artifact (`BYTE_IDENTICAL … bytes sha256 …`), a determinism line
per side, and `PARITY OK`. On any difference it prints a `MISMATCH` block with both sizes and
digests plus a unified diff (the first differing bytes are shown when the files are not UTF-8), so
the failure names the record that drifted. `--repeat N` runs the comparison N times and requires a
single digest per side, which is how the gate detects non-determinism instead of just inequality.

Exit codes: `0` byte-identical, `1` a difference or an unstable repeat, `2` setup failure (a CLI is
missing or could not run). Scratch output goes to the gitignored `build/wasm-parity/` tree.

## 4. CI lane

`.github/workflows/webgpu.yml` gains a `wasm-parity` job next to `wasm-build`. It reuses the same
emsdk cache and install steps (cache key on `tools/emsdk-version.txt`) and then:

1. configures `wasm-webgpu` and builds the `wasm-compiler` preset (the compiler only — not the
   WebGPU targets, which `wasm-build` already covers),
2. configures a minimal native Release build (`tests`, PoC and Vulkan off) and builds only
   `tessera_codegen_cli`, so the job pays for one native compiler binary, not the whole native tree,
3. runs the parity script with `--repeat 2`.

The Wasm preset has `TESSERA_BUILD_TESTS=OFF`, so this cannot be a ctest case in that preset; it is
a script invocation, which also makes the same command runnable on a developer box.

## 5. Verification and status

The Emscripten pin is `7.0.0` (`tools/emsdk-version.txt`), moved from 6.0.13 by owner direction for
this stage. The move is low risk because emsdk 7.0.0 bundles the same emdawnwebgpu port release and
SHA-512 (`v20260423.175430`) that 6.0.13 carried, so the WebGPU bindings are unchanged; the
`wasm-webgpu` targets and the Node smoke were re-verified on the new pin rather than assumed.

Verified on a Linux host (2026-11-15) with emsdk `7.0.0` — the pin in `tools/emsdk-version.txt` —
building from a clean `wasm-webgpu` configure:

- `wasm-webgpu` build preset: the three WebGPU targets build and `webgpu_wasm_smoke` prints
  `webgpu_wasm_smoke: ok`, unchanged.
- `wasm-compiler` build preset: `tessera_codegen.js` + `tessera_codegen.wasm` build with
  `LINK_FLAGS = -fwasm-exceptions -sNODERAWFS=1 -sENVIRONMENT=node`.
- Parity, native CLI versus the Wasm CLI under `$EMSDK_NODE`: the IR is 4542 bytes with SHA-256
  `fa9989b12293632e4ec73c9da718a7524e3423445ff572c0c065fc3270f52fa9` on both sides, and the four
  generated files match too. Three repeated gate runs each reported `unique_digests=1` per side.
- `cmp` on the two IR files exits 0 (`BYTE_IDENTICAL_IR`), so the equality does not depend on the
  script's own comparison code.

Not verified: a browser build of the compiler (P0 is a Node tool by design), the pin on the hosted
GitHub runner (the lane had not run when this note was written), and IR parity beyond the one P0
sample — the gate accepts `--sample` and `--repeat` so later stages can widen the corpus without new
code.

## 6. What P1 and later reuse

P1 (the diagnostics-only page) can call the same lowered module from a browser without
`NODERAWFS`, because its input arrives as an in-memory string rather than a path; the Node tool and
the browser tool then differ only in their file seam. P2's evaluator and P3's WebGPU canvas build on
the same `wasm-webgpu` configure preset. The parity gate itself stays the reference: every later
stage that claims "matches native" should name the native artifact it compared against, the way this
stage names the IR digest.
