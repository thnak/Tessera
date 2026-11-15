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

## 7. P2: evaluator, scene and layout on the null RHI

P2's acceptance is **"the Wasm scene snapshot equals the native harness snapshot for the parity
samples"**. The artifact is the one the harness already produces — the canonical `UiSnapshot`
document of [`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 5.2, written by
`snapshot_to_json()` (`include/tessera/ui/automation/snapshot_json.hpp`). P2 adds no second snapshot
format; it adds an in-memory path *to* that format.

### 7.1 What runs, and what does not

The driver is `tools/codegen/scene_preview.cpp`, the sibling of `diagnose.cpp` (P1). It takes one
in-memory source and runs the CLI's stage order:

```text
source text -> jsx parse -> analyze -> lower -> assign_identity(salt)
            -> evaluator: evaluate each useState/useAnimatedValue initializer into a Scope
            -> build the ui::Node tree (scene_build.cpp)
            -> resolve computed style + Yoga layout + apply_layout (scene_layout.cpp)
            -> ui::automation::snapshot_scene() -> snapshot_to_json()
```

Nothing in that path reads the filesystem, creates a device or RHI object, touches a clock, or
depends on the locale. The null-RHI claim is exactly this: layout stops at the scene's bounds and
never reaches for a render target, so the same code runs on any lane and in the module.

The tree is built on the **same `ui::Node` surface generated C++ uses** — `set_element_kind`,
`set_automation_id`, `set_element_id`, `set_class_name`, `set_role`, `set_aria`,
`set_inline_style`, `set_paint_data`, `set_focusable`, the scroll flags, `add_child` — in the order
`emit_node_attributes.cpp` and `emit_node_behavior.cpp` apply them. A preview tree is therefore the
tree a native build would have built for the same IR, not a parallel model of it.

**Not evaluated by this slice**, and reported as `TESSERA_SCENE_UNSUPPORTED` with an empty `nodes`
array rather than approximated: user component instances (`NodeKind::Component`, which needs prop
binding and slot splicing), `CustomElement`, virtual collections, `Slot` positions, conditional and
repeated children, and inline-span `Text`. `samples/compiler-demo/Demo.tsx` uses a conditional child
and `samples/gallery/Gallery.tsx` imports a user component, so **neither shipped sample is in the
parity corpus**; the corpus is the static subset the preview does evaluate. Widening it is P4/P7
work, not a P2 regression. Dynamic inline-style values are likewise not applied (generated
`apply_state` owns those); the preview lays out the component's initial tree.

### 7.2 The shared walk (one implementation, not two)

The scene → snapshot walk used to live inside `src/harness/engine_host_snapshot.cpp`. It now lives in
`include/tessera/ui/automation/scene_snapshot.hpp` + `src/ui/automation/scene_snapshot.cpp`
(`tessera_automation_scene`), and the harness calls it too:

- `snapshot_scene(const ui::Node&, const SceneSnapshotOptions&)` returns
  `std::expected<UiSnapshot, SceneSnapshotError>`; the caps (`kMaxSnapshotNodes` 65536,
  `kMaxSnapshotDepth` 64, `kMaxSnapshotPathBytes` 256) and the derived structural paths moved with it,
  and `include/tessera/automation/engine_host.hpp` now re-exports the constants as `using` aliases so
  its public names did not change.
- A capped tree is a **hard failure**, never a truncated document: a partial snapshot would compare
  equal to a partial snapshot from the other side and pass a gate it should fail.
- Source-level identity is a callback (`SceneAnnotationFn`), not the automation lane's
  `NodeAnnotations` map, so the header stays free of that module; the harness binds its manifest
  through it.
- `tessera_automation` remains a leaf that links nothing. The walk is its own small target
  (`tessera_automation_scene` = `tessera_automation` + `tessera_scene`) because it reads a live scene.

### 7.3 The two artifacts and the gate

| Side | Artifact | Invocation |
|---|---|---|
| native | `tessera_scene_preview` | `--scene-json OUT --file FILE --salt SALT FILE.tsx` |
| Wasm | `tessera_scene_preview.js` | `tessera_scene_json(source, file, component)` + `tessera_free` |

Both are the same driver. The browser module follows P1's ABI convention exactly: **the module owns
the buffer it returns and the caller hands it back through `tessera_free`** (not a static buffer,
which the next call would invalidate). `src/compiler/codegen/CMakeLists.txt` compiles the sources as
an **OBJECT library on host lanes** (`tessera_scene_preview_entry`, so the entry point is
type-checked on every push) and as a `--no-entry` Emscripten module only under `EMSCRIPTEN`. It is
*not* a verb on `tessera_codegen_cli`: that target is the Node tool the `wasm-compiler` preset builds
for P0's IR parity, and linking the engine into it would drag Yoga and the text stack into that
artifact. The driver is shared instead of the binary, and there is a second executable because the
comparison needs a native command.

`scripts/ci/scene_snapshot_parity.py` copies the shape of `wasm_diagnostics_parity.py`: it runs both
sides over one corpus and requires **byte-identical** documents, decodes both to check
`snapshotVersion`, and — the anti-vacuity rule — every case declares a **minimum node count**, so
"both empty" cannot pass. Both sides are executed with `cwd` set to a scratch case directory, so the
script resolves the CLI, module and Node to **absolute** paths first (P1 shipped a relative-path bug
that failed on CI). The salt and the file name are passed to *both* sides because both are inputs to
every `uiId` (section 2 of the ids spec); `--file` exists precisely so a Windows `\` cannot give a
document no Linux run could reproduce.

`scripts/ci/scene_snapshot_native.py` is the half that runs without Emscripten: it imports the gate's
corpus and salt (so the two cannot drift), and asserts the version, the minimum counts, the
zero-node cases, byte determinism across two runs, and that an explicit `automationId` reaches the
document. A lane without `EMSDK` proves its native half with it.

### 7.4 Verification and status

Verified on this Windows host (2026-11-15), native side only:

- `python -B scripts/ci/scene_snapshot_native.py --native build/.../tessera_scene_preview.exe`:
  `NATIVE CORPUS OK`, 11 cases, node counts 5/4/3/4/4/6/6/3/3/0/0.
- `ctest -R "scene|layout|parity|automation|eval|snapshot"`: 34/34 pass, including the new
  `scene_snapshot` test (`tests/unit/scene_snapshot_tests.cpp`), which pins preorder + parent
  indices, derived paths, the annotation override, hidden-subtree omission, both caps, and the
  document bytes.
- The full suite (555 tests) and `scripts/check_source_size.py` / `scripts/gates.py --only docs` are
  clean.

**CI-only, and why:** the Wasm half. `$env:EMSDK` is empty on this box and P1 confirmed Emscripten
cannot be built here, so `tessera_scene_preview_wasm` was authored to the P1 pattern and is
type-checked by the host OBJECT-library build, but the module itself has **not** been executed. The
byte comparison in `scene_snapshot_parity.py` — and therefore P2's acceptance sentence — is proven
only when the new `wasm-parity` step runs in CI. That is stated rather than implied.

