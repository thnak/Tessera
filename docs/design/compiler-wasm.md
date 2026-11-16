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

## 8. P3: the WebGPU canvas

P3's acceptance is **"the canvas render is within tolerance of the lavapipe golden for the parity
samples"**. It is one sentence about two artifacts, and the two are produced by different machines, so
this section is mostly about which half is proven where — deliberately, because the tempting shortcut
(Node loads the module, compares pixels) is not available and a fake version of it would be worse than
no gate.

### 8.1 One pipeline, three consumers

P2 ended at the scene's bounds. P3 needs the same laid-out tree twice more (recorded as a draw list,
and painted through an RHI device), so the stage sequence moved out of `scene_preview.cpp` into
**`tools/codegen/scene_prepare.cpp`**:

```text
source text -> jsx parse -> analyse -> lower -> assign_identity(salt)
            -> pick component -> evaluate initial state -> build the ui::Node tree
            -> compiled stylesheet -> Yoga layout -> apply_layout
```

`prepare_scene()` returns a `PreparedScene` (the lowered IR the tree borrows its strings from, the
root node, and the stylesheet the layout resolved against). Three consumers walk that one value:

| Consumer | File | Artifact |
|---|---|---|
| P2 snapshot | `scene_preview.cpp` | the canonical `UiSnapshot` document |
| P3 native frame | `scene_frame.cpp` (`scene_frame_render`) | a PNG through an RHI device |
| P3 browser frame | `scene_frame.cpp` (`scene_frame_paint`), called by `wasm_canvas_entry.cpp` | a WebGPU frame on a canvas-owned target |

`scene_preview.cpp` is now a thin wrapper, so the P2 parity gate keeps comparing exactly what it
compared before — verified byte-identical on the same corpus.

The paint half is `ui::paint::paint_scene()` through `Batched2DSink`/`Batched2DRenderer`, the same
objects the native window driver and the golden harness use. A preview frame and a native frame are
therefore the same command stream by construction, not by agreement between two implementations.

**What a preview frame does not paint, and why.** The preview module ships no font and no
`AssetBundle`, so a text run is counted in `text_skipped` and a vector/image payload in
`vectors_skipped`/`images_skipped`; the node's box is still painted. This is a missing *input*, not a
missing capability, and it is reported rather than approximated. The parity corpus below has no glyph
runs, so it does not affect the render comparison.

### 8.2 The preview stylesheet now paints

P2's sheet gave the intrinsic element kinds a size and nothing else, which was right for layout and
wrong for a render gate: a sheet that paints nothing leaves both images a single clear colour, and two
clear-colour images are byte-identical — the render equivalent of "both sides returned `{"nodes":[]}`".
The sheet now carries background colours and one `border-radius`, and the gate declares a **minimum
draw-call count** per case, the same anti-vacuity rule P2 applies to node counts.

The clear colour is the app window background (`rgba8(11, 18, 32)`, `detail::frame_clear_color()` in
`src/app/window_internal.hpp`), not black, so a preview and the native window of the same stylesheet
start from the same pixel everywhere the sheet paints nothing.

### 8.3 What can and cannot be proven, and by what

| Half | Where it runs | What it proves |
|---|---|---|
| `prepare_scene` + frame command stream + report | `scene_frame` ctest, any host | the path builds, lays out, records draws, is deterministic, and the counters are not vacuous |
| lavapipe reference render | `webgpu.yml` `canvas-native-parity` (Ubuntu + `mesa-vulkan-drivers`) | the reference images exist, are deterministic across processes, and meet their minimum draw counts |
| WebGPU candidate render | **nowhere automatically** | needs a browser with a real WebGPU device |
| the pixel comparison itself | `tessera_scene_frame_compare`, any host | the two PNGs are within `webgpu_vs_lavapipe` |

**The honest statement.** The pixel acceptance cannot be proven on this machine or on a hosted CI
runner. Three independent facts block it, and none is a matter of effort:

1. **Node has no WebGPU.** `webgpu_wasm_smoke.cpp` records this explicitly: `navigator.gpu` does not
   exist in Node, so a module loaded there cannot create an adapter, and no frame can be drawn. This
   is not "not yet wired" — there is no device to draw with.
2. **A browser needs a GPU.** The WebGPU candidate is a browser artifact. Hosted runners have no GPU,
   so a headless browser there would fall back to a software adapter or refuse; either way the frame
   would not be the frame a user sees, which is what the acceptance is about.
3. **The golden must be lavapipe** (ADR-009). A hardware driver's pixels are one vendor's rasterizer
   and cannot be a reference. The dev box used for this slice has an RTX 4060 and no lavapipe
   manifest, so `--write-goldens` **refuses** there rather than committing hardware pixels.

So the slice lands three things and names the fourth as remaining:

- (a) the whole native path, proven by `scene_frame` (5 cases, deterministic, non-vacuous) and by the
  `canvas-native-parity` job rendering the lavapipe reference;
- (b) the canvas page, the device plumbing, render-on-demand, and the unsupported-browser path;
- (c) the node-count-equivalent of a pixel gate: the compare tool plus the script that would apply it;
- (d) **remaining**: the WebGPU candidate. It needs a real browser with a real device. That is a new
  CI dependency (Playwright-class, plus a GPU runner), which is a spec/ADR decision — `docs/specs/
  dependencies.md` and `THIRD_PARTY_NOTICES.md` entries, and probably an ADR — so it is reported, not
  added. The concrete plan: a browser job that serves `build/pages`, runs `canvas.html`, calls
  `tessera_canvas_frame`, reads the canvas back with `toDataURL()`, and feeds the PNG directory to
  `scene_frame_parity.py --mode compare`. Every piece of that exists except the browser.

Measuring the delta between a hardware WebGPU frame and a lavapipe golden is exactly what
`tests/golden/tolerance.json`'s `webgpu_vs_lavapipe` pair (8/255 per channel, 2% of pixels, SSIM >=
0.99, deltaE <= 2.0) was written for, so the browser lane adds no policy, only a device.

### 8.3a How the frame reaches the canvas

The page owns the surface, the module owns the drawing:

1. `canvas.js` requests the device, then turns the JS `GPUDevice`/`GPUQueue` into Emscripten
   handles with `WebGPU.importJsDevice` / `importJsQueue` (the module exports `WebGPU` for this)
   and calls `tessera_webgpu_set_device`, which returns 1, 0 (null handle) or -1 (wrapping threw).
   Passing the JS objects themselves does not work: they coerce to 0 and read as a null handle.
2. It configures the canvas context once (`rgba8unorm`, opaque).
3. Each frame it imports `getCurrentTexture().createView()` with `importJsTextureView` and passes
   that handle to `tessera_canvas_frame`. The module wraps it in `create_canvas_target`, paints the
   scene into it with `scene_frame_paint`, and releases the view when the target is destroyed.
   A null view draws offscreen instead, which is what the Node probe uses.

Both target kinds derive from `WebGpuTargetViews`, and the command list casts to that. A cast to the
concrete offscreen class made a canvas target read the wrong memory and begin a pass with no view.
The preview module ships no font, so text nodes paint their boxes only (`textSkipped`).

### 8.4 Render on demand

The page does not run a frame loop. `canvas.js` draws once per *change*: a Run, a device-pixel-ratio
change, or the tab becoming visible again. `requestAnimationFrame` is the scheduling primitive — it
coalesces a burst of notifications into one frame and aligns the draw with the display — but nothing
re-arms it after a frame, so a static scene stops drawing. This is the browser form of the repository's
idle rule (a static UI presents nothing), and it also means the page does not spin a GPU at 60 Hz on a
tab nobody is looking at.

### 8.5 The unsupported path

`canvas.html` ships the unsupported state as its *default* pane content and `canvas.js` only replaces
it with the canvas once a device exists. The three ways to get there — no `navigator.gpu`, no adapter,
`requestDevice()` rejecting — each name their cause and link `scene.html`, which needs no GPU. A module
that fails to load, or one without the canvas exports, reports that too. A blank canvas is never the
outcome.

`scene_frame_parity.py --mode canvas-report` checks the same contract without a browser: it loads
`tessera_canvas.js` under Node (which has no `navigator.gpu`, exactly like a browser without WebGPU)
and requires a `hasDevice:false` report rather than a crash.

### 8.6 Verification and status

Verified on this Windows host (2026-11-16), native side only:

- `ctest -R scene_frame`: passes. Covers `prepare_scene` (and the failed-prepare shape), the null
  backend's command stream and counters, byte-identical pixels across two runs, the report's JSON
  escaping and the no-device/failed-frame distinction, the parity policy (identical pair passes;
  moved geometry, flat-region drift and a size difference fail; an edge rounding difference passes),
  and the SPIR-V-free pipeline description.
- `python -B scripts/ci/scene_frame_parity.py --native .../tessera_scene_frame.exe`: `NATIVE
  REFERENCE OK`, 5 cases, each deterministic across two processes and above its minimum draw count.
- `tessera_scene_frame --backend vulkan` on an RTX 4060 renders the corpus (e.g. 5 nodes, 1 draw, 24
  vertices) and is byte-identical across runs; `--backend null` records the same counts. The RTX
  render is evidence the path works, **not** a golden, and the script refuses to save it as one.
- `tessera_scene_frame_compare` classifies an identical pair as a pass (`ssim=1 deltaE=0`) and two
  different scenes as a structural failure (`mismatch=0.207 ssim=0.879`), so the comparison is not
  vacuous.

**CI-only, and why:** the browser half. `$env:EMSDK` is empty on this box, so
`wasm_canvas_entry.cpp`, `webgpu_canvas_target.cpp` and `canvas.js` have not been compiled or run;
the host lanes type-check `scene_frame.cpp` and `canvas_report.cpp` as OBJECT libraries, which is the
part they can check. The Emscripten build and the no-device contract run in `pages.yml` / `webgpu.yml`.
**The pixel comparison has not run anywhere yet** — see 8.3.

### 8.7 Files

| Path | Role |
|---|---|
| `tools/codegen/scene_prepare.{hpp,cpp}` | the shared stage sequence (`prepare_scene`) |
| `tools/codegen/scene_frame.{hpp,cpp}` | paint into an RHI target; `scene_frame_render` adds offscreen+readback |
| `tools/codegen/scene_frame_main.cpp` | `tessera_scene_frame`: renders a PNG (vulkan or null) |
| `tools/codegen/scene_frame_compare.cpp` | `tessera_scene_frame_compare`: the pixel comparison, one policy |
| `tools/codegen/canvas_report.{hpp,cpp}` | the JSON report the page parses (host-compilable) |
| `tools/codegen/wasm_canvas_entry.cpp` | the browser ABI: device handover + `tessera_canvas_frame` |
| `src/rendering/backend/webgpu/src/webgpu_canvas_target.cpp` | an RHI target over a host-owned canvas texture view |
| `designer/web/{canvas.html,canvas.js,canvas.css}` | the page |
| `scripts/ci/scene_frame_parity.py`, `scene_frame_canvas_probe.mjs` | the gate: native reference, node contract, comparison |
| `tests/unit/scene_frame_tests.cpp` | the native half, as a ctest case |

## 9. P4: browser input, and state kept across edits

P4's acceptance is **"scripted clicks give the same state cells as the parity runner"**. The artifact
is the state cell list the preview parity runner already compares: `ui::StateSnapshot`
(`include/tessera/ui/scene/state_snapshot.hpp`), the same `std::vector<ui::StateCell>` a generated
`snapshot_state()` returns and `tests/parity/parity_runner.cpp` reads through `parity::NativeSide`.
P4 adds no second notion of state.

### 9.1 What a synthetic click actually invokes

The preview is an IR *evaluator*. It does not contain the developer's C++ and cannot run it. A click
goes through the engine's own router:

```text
PreviewInput -> ui::InputEvent -> ui::InputRouter::dispatch(root, event)
             -> hit test -> implicit pointer capture -> press/release
             -> ClickSynthesizer: nearest common ancestor -> ui::EventType::Click
             -> W3C capture -> target -> bubble (src/ui/scene/input_dispatch.cpp, event.cpp)
             -> Node::bind_event callback installed by scene_session.cpp
             -> eval::Evaluator::run(handler actions, Scope) -> state cells
```

Everything up to the callback is the production code path, unchanged: hit testing, capture, the
common-ancestor click rule, focus, and capture/target/bubble propagation are `src/ui/scene/`, not a
preview copy. The callback then runs the handler's **TSX preview body** through the evaluator, the
same action list `parity::run_parity` executes for a `click <automationId>` step. Consequently:

- a handler that is a native `bind::` binding with no `preview` body runs **no** actions, exactly as
  in the parity runner, because its body is the developer's C++;
- a handler that declares a payload parameter (`(e: ClickEvent) => ...`) **cannot** run: the
  evaluator has no record value to bind to the parameter. It is reported in the result document's
  `unrun` list and counted by the gate, never silently treated as "nothing happened";
- real handler execution in the browser is the ADR-046 web target's job (the same code path as
  native, the browser being one OS behind the HAL), not this slice's.

### 9.2 State kept across edits: the exact rule

`tools/codegen/scene_state.hpp` states it in full; the operative sentence is:

> A state cell is carried across an edit when, and only when, the new evaluation has a component with
> the same **component id** that declares a cell with the same **cell id** (`state::count`) and the
> same **IR type**. Everything else starts at its initializer.

- The component id is the salted hash of source path plus component name
  ([`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 2.1), assigned by
  `ids::assign_identity`. It is a pure function of the text, so two separate evaluations of edited
  text are directly comparable. Inserting, deleting or reordering components cannot hand one
  component's state to another, and a moved file is a different component.
- The cell id, not a position, is the cell identity: reordering `useState` calls or inserting one
  above keeps every existing value; renaming a variable makes it a different cell.
- The IR type is part of the key because the generated C++ field's type comes from it: carrying an
  `int` into a `float` cell would be a conversion the native build never performs.
- The **initializer expression is deliberately not** part of the key, so editing `useState(0)` to
  `useState(10)` does not reset a live counter. `build_scene()` seeds only the cells a carried scope
  does not already hold, so a carried value is never recomputed.
- A removed component's cells carry a component id the new evaluation does not have, so they are
  dropped (`dropped_component`) and never offered to a component that took its place.

**Not in the key yet, and why:** the list-item `key` dimension of `(uiId, itemKey)`
([`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 1). This preview reports a repeated
child as unsupported (section 7.1), so no keyed component instance can exist. When loops are evaluated
the key extends to `(componentId, itemKey, cellId)`; adding it only narrows the matches, so a value
carried correctly today stays correct. That extension — and whether a page may ever persist cells
across a reload — is an ADR/protocol question: `docs/specs/designer-protocol-v1.md` section 14 states
V1 has **no** state snapshot or restore, and this slice does not add one. The carry is in-process,
between two evaluations in one session.

### 9.3 The artifacts and the gate

| Side | Artifact | Invocation |
|---|---|---|
| native | `tessera_scene_preview` | `--input-json OUT --script SCRIPT --file FILE --salt SALT FILE.tsx` |
| Wasm | `tessera_scene_preview.js` | `tessera_input_json(source, file, component, script)` + `tessera_free` |

The result document is `{"inputVersion":1,"initial":[...],"steps":[...]}`; each step carries `found`,
`dispatched`, the `ran` and `unrun` handler ids, and the state cells after it. Cells are the existing
`ui::StateCell` shape, with `kind` and `value` as text so a diff names the cell that moved and never
has to reason about JSON number formatting.

`scripts/ci/scene_input_parity.py` copies `scene_snapshot_parity.py`'s shape: one corpus, both sides
executed, **byte-identical** documents, absolute paths resolved before any `cwd` change, and an
anti-vacuity rule. For input the vacuity that matters is different: every case declares a **minimum
state-cell count and a minimum number of handlers that must run**, and any non-empty `unrun` fails, so
"both sides clicked nothing" cannot pass. `scripts/ci/scene_input_native.py` is the half that runs
without Emscripten; it imports the gate's corpus and salt so the two cannot drift.

`tests/parity/scene_input_tests.cpp` is the acceptance test: it hands the scene session to the real
`parity::run_parity` as that runner's **native side**, so "scripted clicks give the same state cells
as the parity runner" is checked by the runner itself rather than by a second, weaker comparison.

### 9.4 Verification and status

Verified on this Windows host (2026-11-15), native side only:

- `ctest -R scene_input`: passes. It covers click-versus-runner parity over three clicks, that a click
  on an unknown id or outside the viewport changes no cell, that `state::count` **survives** an edit
  that removes a sibling cell and changes the initializer, that a removed component's value is **not**
  handed to a same-shaped component (`Other` starts at 0, not at `Counter`'s 1), that a type change
  drops the cell, the script grammar, and the document shape.
- The identity rule was mutation-tested: removing the component-id comparison in `apply_state()` made
  six assertions fail, including `Other` starting at 1 instead of 0. The test is not vacuous.
- `python -B scripts/ci/scene_input_native.py --native build/.../tessera_scene_preview.exe`:
  `NATIVE CORPUS OK`, 6 cases, including a click that bubbles to a parent handler, a click on a node
  with no handler, and intermediate state threading (totals 2, 5, 9).

**CI-only, and why:** the Wasm half, for the same reason as section 7.4. `$env:EMSDK` is empty on this
box, so `tessera_input_json` is authored to the P1/P2 pattern and type-checked by the host build but
has not been executed. The byte comparison in `scene_input_parity.py` — and therefore P4's acceptance
sentence — is proven when the Wasm lane runs it.

## 10. P5: share links, the sample gallery and the environment toggles

P5's acceptance is **"a compressed URL-fragment link round-trips; the samples from `samples/` load
within the project-load budget"**. The format is normative in
[`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 7.1; this section records what is
implemented, what is proven locally, and what only CI can prove.

### 10.1 The pieces

| Piece | Where | Why it is there and not in a page |
|---|---|---|
| Codec | `designer/web/share.js` | Pure ES module: no DOM, no `fetch`, no Wasm. It is what makes the round trip testable in Node, and the page and the test import the same bytes |
| State vocabulary | `designer/web/preview-state.js` | The constants and validation, shared by the codec and the toolbar; keeping it separate is what lets the codec be tested without the DOM |
| Page chrome | `designer/web/studio.js` | Toolbar, share button and the broken-link banner, mounted once per page. Both pages need exactly this and would otherwise carry two copies |
| Gallery | `designer/web/gallery.js` | Manifest fetch, sample fetch and the derived titles, split out of `studio.js` so the chrome module stays under the size limit. Lazy: nothing runs until the gallery is opened |
| Manifest | `designer/web/samples.json` | Generated from `samples/**/*.tsx` by `scripts/ci/gen_sample_manifest.py`; committed so a deploy runs no generator step. The page around it is built by Vite since ADR-054 |
| Sized layout export | `tools/codegen/wasm_scene_entry.cpp` | `tessera_scene_json_sized(...)`; the P2 signature is untouched because the parity gate pins its defaults |

### 10.2 What the toggles actually do

`viewport` and `dpr` are real layout inputs: `tessera_scene_json_sized` passes them to
`ScenePreviewOptions`, so the scene page re-lays-out at the chosen rectangle. Measured on this host
with the native CLI at 320×240 / 390×844 / 1280×800: the root node's bounds follow the request
(`--size 390x844` gives a 390×844 root) and the node count is unchanged, which is the difference
between "the toggle is wired" and "the toggle re-renders the same picture".

`theme` is carried in the link and persisted locally, but it is **not applied**: the null-RHI
snapshot path has no theme asset, so a themed preview would be a claim the module cannot keep. That
is a spec-level gap, not a bug in this slice, and it is recorded rather than papered over.

### 10.3 Verification and status

Verified on this Windows host (2026-11-15):

- `node scripts/ci/preview_share_roundtrip.mjs`: 18 cases, all pass. Covers the compressed (`z`),
  gzip (`g`) and uncompressed (`u`) modes, every viewport × DPR × theme combination, multi-byte and
  empty sources, determinism, the whole-URL and bare-fragment inputs, the version refusal, the
  malformed and invalid-field refusals, the 8 KiB fragment cap in both directions, the
  decompression-bomb cap, and `samples/parity-demo/Counter.tsx` encoding to 584 characters.
- `node scripts/ci/preview_studio_smoke.mjs`: 9 stub-DOM cases, all pass. Covers lazy manifest fetch
  (nothing before the gallery opens), one fetch per page load, a gallery pick reaching the textarea,
  the toolbar's option sets, local persistence (environment only, never the source), the share link
  through the address bar, a good link restoring source/viewport/theme, and a broken link showing its
  code in the banner.
- `node scripts/ci/preview_page_checks.mjs`: 5 static checks, all pass: every element `studio.js` looks
  up exists in both pages, `samples.json`'s four paths exist, and neither page calls a function it does
  not declare.
- `python -B -m unittest discover -s scripts/tests`: 239 tests, OK, including 14 new cases for the
  manifest generator.
- `python -B scripts/check_source_size.py`: 0 errors; `scripts/gates.py --only docs`: PASS.

**A real bug the smoke test found, and why it is worth naming:** the gallery's first version read
`aria-expanded`, wrote it back unchanged, then hid the panel on `!open`, so the button never opened
anything. A stub-DOM test caught it; nothing in the codec test could have. Four more defects turned up
the same way (a missing `ShareError` import that would have thrown on every broken link, a `dpr` that
was a number before a click and a string after, a gallery note node detached from the container, and
a broken link leaving an empty editor instead of the page's sample).

**CI-only, and why:** the browser half. The Wasm module cannot be built on this box (`$env:EMSDK` is
empty), so `tessera_scene_json_sized` has **not** been executed as Wasm; it is type-checked by the
host OBJECT-library build, and the Pages lane greps the built module for the export. The page itself —
that `index.html`/`scene.html` render, that the fragment survives a real navigation, that
`CompressionStream` behaves as expected in a browser — is verified by the deploy and by opening
https://thnak.github.io/Tessera/. The Node tests are honest about being codec and stub-DOM tests, not
browser tests.

**§11: the merge into one page.** The three pages described above are now one `index.html` with three
views; the module layout, the old-URL redirect that keeps `#p5=` links working, and the P6 re-pin are
in [`studio-page.md`](studio-page.md).
