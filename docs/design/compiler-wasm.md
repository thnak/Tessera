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
must never apply to a sample app or any browser-shipped artifact — hence a `target_link_options` on
that one target under `if (EMSCRIPTEN)`, not a directory or project-wide option.

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
2. **Generated C++** (`--write`), unless `--ir-only` is passed: `Counter.g.hpp`, `Counter.hpp`,
   `Counter.cpp` and `automation.manifest.json`, each byte-compared and the file *sets* compared
   too (a silently dropped artifact fails). Comparing codegen's output extends the gate from the
   front end to the artifacts the preview pipeline consumes downstream; it was byte-identical when
   the gate was written, so the assertion is drift coverage rather than an aspiration.

On success: one line per artifact (`BYTE_IDENTICAL . bytes sha256 .`), a determinism line per
side, and `PARITY OK`. On any difference: a `MISMATCH` block with both sizes and digests plus a
unified diff (the first differing bytes when the files are not UTF-8), so the failure names the
record that drifted. `--repeat N` runs it N times and requires one digest per side, which detects
non-determinism rather than only inequality. Exit codes: `0` byte-identical, `1` a difference or an
unstable repeat, `2` setup failure. Scratch output goes to the gitignored `build/wasm-parity/`.

## 4. CI lane

`.github/workflows/webgpu.yml` gains a `wasm-parity` job next to `wasm-build`. It reuses the same
emsdk cache and install steps (cache key on `tools/emsdk-version.txt`) and then:

1. configures `wasm-webgpu` and builds the `wasm-compiler` preset (the compiler only — not the
   WebGPU targets, which `wasm-build` already covers),
2. configures a minimal native Release build (`tests`, PoC and Vulkan off) and builds only
   `tessera_codegen_cli`, so the job pays for one native compiler binary, not the whole native tree,
3. runs the parity script with `--repeat 2`.

The Wasm preset has `TESSERA_BUILD_TESTS=OFF`, so this cannot be a ctest case in that preset; it is a
script invocation, which also makes the same command runnable on a developer box.

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
- `cmp` on the two IR files exits 0, so the equality does not depend on the script's own
  comparison code.

Not verified: a browser build of the compiler (P0 is a Node tool by design), the pin on the hosted
GitHub runner (the lane had not run when this note was written), and IR parity beyond the one P0
sample — the gate accepts `--sample` and `--repeat` so later stages can widen the corpus without new
code.

## 6. What P1 and later reuse

P1 (the diagnostics-only page) can call the same lowered module from a browser without
`NODERAWFS`, because its input arrives as an in-memory string rather than a path; the Node tool and
the browser tool then differ only in their file seam. P2's evaluator and P3's WebGPU canvas build on
the same `wasm-webgpu` configure preset. The parity gate stays the reference: every later stage that
claims "matches native" names the native artifact it compared against (the IR digest above; the
scene snapshot in 7.3; the author-stylesheet case in 8.2).

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

Nothing in that path reads the filesystem, creates a device or RHI object, touches a clock or
depends on the locale: layout stops at the scene's bounds and never reaches for a render target, so the
same code runs on any lane and in the module.

The tree is built on the **same `ui::Node` surface generated C++ uses** (`set_element_kind`,
`set_automation_id`, `set_element_id`, `set_class_name`, `set_role`, `set_aria`, `set_inline_style`,
`set_paint_data`, `set_focusable`, the scroll flags, `add_child`) in the order
`emit_node_attributes.cpp` and `emit_node_behavior.cpp` apply them, so a preview tree is the tree a
native build would have built rather than a parallel model of it.

**Not evaluated by this slice**, and reported as `TESSERA_SCENE_UNSUPPORTED` with an empty `nodes`
array rather than approximated: `CustomElement`, virtual collections, nested list
templates, and inline-span `Text`. **User component instances are evaluated** (issue #594): a host `View` holds the callee's tree in its
own scope, `{children}` splices at the `Slot` under the caller's scope. **Not yet inside an instance:**
event handlers, state changes, callback props, dynamic `className`/`role`/`aria-*`. **Conditional and repeated children are evaluated** (issue #389)
through the same evaluator and scope as text and clicks; `docs/design/preview-dynamic-children.md`
states both decisions, the bounded subset and why there is no second evaluator. The gallery sample's
`Button` is an instance, so it paints but is not yet interactive. Dynamic inline-style values
are not applied either (generated `apply_state` owns those); the preview lays out the initial tree.

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
  through it, and `tessera_automation` remains a leaf that links nothing. The walk is its own small
  target (`tessera_automation_scene` = `tessera_automation` + `tessera_scene`) because it reads a live
  scene.

### 7.3 The two artifacts and the gate

| Side | Artifact | Invocation |
|---|---|---|
| native | `tessera_scene_preview` | `--scene-json OUT --file FILE --salt SALT [--stylesheet CSS] FILE.tsx` |
| Wasm | `tessera_scene_preview.js` | `tessera_scene_json(component, fold, width, height, dpr)` over a staged project + `tessera_free` |

Both are the same driver. The browser module follows P1's ABI convention exactly: **the module owns
the buffer it returns and the caller hands it back through `tessera_free`** (not a static buffer,
which the next call would invalidate). `src/compiler/codegen/CMakeLists.txt` compiles the sources as
an **OBJECT library on host lanes** (`tessera_scene_preview_entry`, so the entry point is
type-checked on every push) and as a `--no-entry` Emscripten module only under `EMSCRIPTEN`. It is
*not* a verb on `tessera_codegen_cli`: that target is the Node tool the `wasm-compiler` preset builds
for P0's IR parity, and linking the engine into it would drag Yoga and the text stack into that
artifact; the driver is shared instead, and a second executable exists because the comparison needs a
native command. `scripts/ci/scene_snapshot_parity.py` copies `wasm_diagnostics_parity.py`: one corpus,
both sides executed, **byte-identical** documents, `snapshotVersion` decoded on both, and — the
anti-vacuity rule — a **minimum node count** per case, so "both empty" cannot pass. A case may carry an
author stylesheet (issue #390), and both sides then drive the styled export. Both sides run with `cwd`
set to a scratch directory so the script resolves the CLI, module and Node to **absolute** paths first
(P1 shipped a relative-path bug that failed on CI), and the salt and file name go to *both* sides
because both are inputs to every `uiId`; `--file` exists so a Windows `\` cannot give a document no
Linux run could reproduce.

`scripts/ci/scene_snapshot_native.py` is the half that runs without Emscripten: it imports the gate's
corpus and salt (so the two cannot drift), and asserts the version, the minimum counts, the
zero-node cases, byte determinism across two runs, and that an explicit `automationId` reaches the
document. A lane without `EMSDK` proves its native half with it.

### 7.4 Verification and status

Verified on this Windows host (2026-11-15), native side only:

- `python -B scripts/ci/scene_snapshot_native.py --native build/.../tessera_scene_preview.exe`:
  `NATIVE CORPUS OK`, 11 cases, node counts 5/4/3/4/4/6/6/3/3/0/0.
- `ctest -R "scene|layout|parity|automation|eval|snapshot"`: 34/34 pass, including the
  `scene_snapshot` test (`tests/unit/scene_snapshot_tests.cpp`), which pins preorder + parent indices,
  derived paths, the annotation override, hidden-subtree omission, both caps and the document bytes.
- The full suite (555 tests) and `scripts/check_source_size.py` / `scripts/gates.py --only docs` are clean.

**CI-only, and why:** the Wasm half. `tessera_scene_preview_wasm` follows the P1 pattern and the host
OBJECT-library build type-checks it, but the byte comparison in `scene_snapshot_parity.py` — and
therefore P2's acceptance sentence — is proven only where the `wasm-parity` step runs.

## 8. P3: the WebGPU canvas

P3's acceptance is **"the canvas render is within tolerance of the lavapipe golden for the parity
samples"**: one sentence about two artifacts from different machines, so this section is mostly about
which half is proven where — the tempting shortcut (Node loads the module, compares pixels) is not
available, and a fake version of it would be worse than no gate.

### 8.1 One pipeline, three consumers

P2 ended at the scene's bounds. P3 needs the same laid-out tree twice more (recorded as a draw list,
and painted through an RHI device), so the stage sequence of 7.1 moved out of `scene_preview.cpp` into
**`tools/codegen/scene_prepare.cpp`**. `prepare_scene()` returns a `PreparedScene` (the lowered IR, the
root node, the resolved stylesheet and its diagnostics), and three consumers walk that one value:

| Consumer | File | Artifact |
|---|---|---|
| P2 snapshot | `scene_preview.cpp` | the canonical `UiSnapshot` document |
| P3 native frame | `scene_frame.cpp` (`scene_frame_render`) | a PNG through an RHI device |
| P3 browser frame | `scene_frame.cpp` (`scene_frame_paint`), called by `wasm_canvas_entry.cpp` | a WebGPU frame on a canvas-owned target |

`scene_preview.cpp` is now a thin wrapper, so the P2 parity gate keeps comparing exactly what it did
before — verified byte-identical on the same corpus. The paint half is `ui::paint::paint_scene()` through
`Batched2DSink`/`Batched2DRenderer`, the same objects the native window driver and the golden harness use,
so a preview frame and a native frame are the same command stream by construction rather than by
agreement between two implementations.

**Text is painted, from font bytes the page supplies (issue #387).** The module still embeds no font:
the page fetches one face from its own origin and hands over the bytes through
`tessera_canvas_set_font(ptr, size)`, which the module gives to the production
`FontFace`/`HarfBuzzShaper`/`TextRenderer` stack (`tools/codegen/preview_text.hpp`). Runs are therefore
shaped and atlased as a native frame draws them, and `textSkipped` is 0 for a sample the face covers.
Before the bytes arrive — and after a failed fetch — the old behaviour stands, so a missing font
degrades rather than breaks the preview. A glyph the bounded atlas cannot place is counted in
`glyphsDropped` and the run still advances, so the layout is unaffected.
**What else a preview frame does not paint, and why.** The module ships no `AssetBundle`, so a
vector/image payload is counted in `vectors_skipped`/`images_skipped` and the node's box is still painted.
This is a missing *input*, not a missing capability, and it is reported rather than approximated; the
parity corpus has no such payloads, so the comparison is unaffected.

### 8.2 The preview stylesheet now paints

P2's sheet gave the intrinsic element kinds a size and nothing else, which was right for layout and
wrong for a render gate: a sheet that paints nothing leaves both images a single clear colour, and two
clear-colour images are byte-identical — the render equivalent of "both sides returned `{"nodes":[]}`".
The sheet now carries background colours and one `border-radius`, and the gate declares a **minimum
draw-call count** per case. The clear colour is the canvas colour (white as a browser's, formerly
navy; `detail::frame_clear_color()`), not black, so a preview and the native window start from one pixel.

**The project is staged, and only imported stylesheets apply (issues #390 and the project preview).** A
module no longer takes one source and one sheet per call. The page stages a whole project with
`tessera_project_begin`, `tessera_project_add_source(path, text)`, `tessera_project_add_stylesheet(path, text)`
and `tessera_project_set_entry(path)` (`wasm_project_exports.cpp`, shared by all three modules), then calls
an export that reads it: `tessera_diagnostics_json()`, `tessera_scene_json(component, fold, w, h, dpr)`,
`tessera_input_json(component, script)`, `tessera_css_diagnostics_json()` and
`tessera_canvas_apply(component, fold, w, h, dpi)`. The entry is the project's own (`tessera.json`
`app.entry`); a project that names none is reported as such, with no fallback to another file.
`evaluate_project()` walks from the entry through its relative imports (`project_walk.hpp`), so the
stylesheets that apply are exactly the imported ones, in first-import order, exactly as a web import graph
cascades. `prepare_preview_sheets()` (`scene_stylesheet.cpp`) decides the sheet: any rule in an imported
sheet means the author's text and the built-in sheet is **not** compiled under it, while no imported sheet
with a rule keeps `preview_stylesheet()` as the fallback, so the parity corpus keeps today's bytes. A
diagnostic carries the file it is in (`jsx::Diagnostic::file`), so an error in `ui/Home.tsx` is not
reported against the entry.

A sheet with one bad rule is still the author's sheet, so an invalid declaration cannot resurrect the
built-in blue `Button`. `style`, `className` and `var()` take the ordinary cascade
(`style::SceneStyleTree::compute()`); the preview invents no tokens.
`tessera_canvas_draw` takes no project (see 8.3a): it paints the tree the last apply built. The frame
report carries an appended `cssDiagnostics` array; `tessera_css_diagnostics_json` is `cssVersion: 2`
(every problem names its own file).

**Diagnostics are reported, never fatal.** A CSS problem is a record with the spec's `TESSERA_CSS_*` code
and a 1-based line/column into the CSS text (`author_stylesheet_diagnostics()`); the tolerant parser drops
the bad rule and the frame is still drawn. A stack overflow in Wasm aborts the module — losing a designer's
unsaved work — so the recursion budgets are proven, not assumed: #410 bounded nested selectors (16) and
at-rules (32), and #390 added the `@supports` condition bound (32) that #410 left open, where 20000 nested
`not`s or `(`s crashed `tessera_codegen` with 0xC00000FD. `scene_stylesheet` ctest feeds all four grammars,
thousands deep plus a 7 KB hostile sheet, through the preview path and requires a diagnostic and a
laid-out frame, so the bound is asserted where a person sees it.

### 8.3 What can and cannot be proven, and by what

| Half | Where it runs | What it proves |
|---|---|---|
| `prepare_scene` + frame command stream + report | `scene_frame` ctest, any host | the path builds, lays out, records draws, is deterministic, and the counters are not vacuous |
| glyph runs from shipped font bytes | `scene_frame` ctest, any host | text paints with a font, is skipped without one, and the atlas reports what it drops (#387) |
| lavapipe reference render | `webgpu.yml` `canvas-native-parity` (Ubuntu + `mesa-vulkan-drivers`) | the reference images exist, are deterministic across processes, and meet their minimum draw counts |
| device handover and a drawn frame | `pages.yml`, headless Chromium + SwiftShader (`--enable-unsafe-webgpu`) | the page's JS device reaches the module as a real handle and the frame has draw calls (#375); both unsupported headings name the right side |
| WebGPU candidate render as a *pixel* reference | **nowhere automatically** | needs a browser with a real WebGPU device; SwiftShader is not one |
| the pixel comparison itself | `tessera_scene_frame_compare`, any host | the two PNGs are within `webgpu_vs_lavapipe` |

**The honest statement.** The *handover* is now a CI gate: a hosted runner can give headless Chromium a
SwiftShader adapter and device, which is enough to prove the page's device reaches the module and the
canvas draws (#375, `preview_browser_canvas_handover.mjs`). The *pixel* acceptance still cannot be
proven: SwiftShader is a software renderer, not "a WebGPU frame the user would see", and the golden must
be lavapipe (ADR-009), whose manifest this box lacks, so `--write-goldens` **refuses**. **Remaining** is
that candidate: a real GPU runner draws into the page's canvas, reads it back and feeds
`scene_frame_parity.py --mode compare` at `tests/golden/tolerance.json`'s `webgpu_vs_lavapipe` pair.

### 8.3a How the frame reaches the canvas

The page owns the surface, the module owns the drawing:

1. `canvas.js` requests the device, then turns the JS `GPUDevice`/`GPUQueue` into Emscripten handles with
   `WebGPU.importJsDevice` / `importJsQueue` (the module exports `WebGPU` for this) and calls
   `tessera_webgpu_set_device`, which returns 1, 0 (null handle) or -1 (wrapping threw). Passing the JS
   objects themselves does not work: they coerce to 0 and read as a null handle. It configures the canvas
   context once (`rgba8unorm`, opaque), then each frame passes a texture view as below.
2. Each frame it imports `getCurrentTexture().createView()` with `importJsTextureView` and passes
   that handle to `tessera_canvas_draw`, which paints the running session into it and releases the view
   when the target is destroyed; a null view draws offscreen, which the Node probe uses. Draw takes
   **no strings**: the tree, and the sheets it was laid out with, are the ones the last `apply` left in
   the module, and its report still carries `cssDiagnostics`. (`tessera_canvas_frame` and `tessera_canvas_run` are gone: one-shot calls cannot carry state.)

Both target kinds derive from `WebGpuTargetViews`, and the command list casts to that. A cast to the
concrete offscreen class made a canvas target read the wrong memory and begin a pass with no view.
The module still ships no font, but a frame now paints text when the page has supplied one: once per
device the page fetches `fonts/IBMPlexSans-Latin.ttf` from its own origin, writes the bytes into the
module's heap and calls `tessera_canvas_set_font(ptr, size)`, after which a draw shapes and draws its
runs through the production text stack. Without that call (and after a failed fetch, which the pane
names) text nodes paint their boxes only, unchanged from before issue #387.
### 8.4 Render on demand

The page does not run a frame loop: it draws once per *change* (an applied edit, a device-pixel-ratio
change, or the tab becoming visible again). `requestAnimationFrame` coalesces a burst of notifications
into one frame and aligns the draw with the display, but nothing re-arms it after a frame, so a static
scene stops drawing — the browser form of the idle rule. Since #444 the edit is the main trigger:
`tessera_canvas_apply` (evaluate, carry, answer `changed`) then `tessera_canvas_draw` only when
`changed`, so unchanged text costs no frame and a failed source keeps the picture. The 150 ms debounce
and this coalescer compose once each, and both reports carry `frames_painted()`, so "one frame per
burst, zero when static" is read rather than asserted.

### 8.5 The unsupported path

The page ships the unsupported state as its *default* pane content and swaps in the canvas only once a
device exists. Each way to get there — no `navigator.gpu`, no adapter, a rejecting `requestDevice()` —
names its cause and points at the Scene tab, which needs no GPU; a module that fails to load, or one
without the canvas exports, reports that too, and a blank canvas is never the outcome.
The heading says which side failed (#375): `data-cause` is `browser` or `module` on `#unsupported`.
`scene_frame_parity.py --mode canvas-report` checks the same contract without a browser: it loads
`tessera_canvas.js` under Node (no `navigator.gpu`, like a browser without WebGPU) and requires a
`hasDevice:false` report, not a crash.

### 8.6 Verification and status

Verified on this Windows host (2026-11-16), native side only:

- `ctest -R scene_frame`: passes. Covers `prepare_scene` (and the failed-prepare shape), the null
  backend's command stream and counters, byte-identical pixels across two runs, the report's JSON
  escaping and the no-device/failed-frame distinction, the parity policy (identical pair passes;
  moved geometry, flat-region drift and a size difference fail; an edge rounding difference passes),
  the SPIR-V-free pipeline description, and - since issue #387 - text painted from shipped font bytes
  (`textSkipped` 0 with a font, counted without one), the same frame twice from the same bytes, a
  refused font leaving a usable degraded frame, and an exhausted atlas counting `glyphsDropped`.
- `python -B scripts/ci/scene_frame_parity.py --native .../tessera_scene_frame.exe`: `NATIVE
  REFERENCE OK`, 5 cases, each deterministic across two processes and above its minimum draw count.
- `tessera_scene_frame --backend vulkan` on an RTX 4060 renders the corpus byte-identically across runs
  and `--backend null` records the same counts; `--font PATH` renders the same sample with glyphs (3
  runs, 6 draws, 138 vertices on `Counter.tsx`), also byte-identical. The RTX render is evidence the
  path works, **not** a golden, and the script refuses to save it as one.
- `tessera_scene_frame_compare` classifies an identical pair as a pass (`ssim=1 deltaE=0`) and two
  different scenes as a structural failure (`mismatch=0.207 ssim=0.879`), so it is not vacuous.
- `ctest -R scene_stylesheet` (issue #390): the accepted cases plus the depth-bound proof of 8.2, and
  the three owner decisions as behavior — empty/comment sheets keep the built-in 120x32 blue, any
  author rule removes it, a `:root` token changes a computed colour through `var()`, and inline `style`
  wins over a class rule. `tessera_scene_preview --stylesheet` agrees with the module on the
  `author-stylesheet` parity corpus case, so the same author CSS produces the same bytes on both sides.

**What runs only in CI:** the *browser* half. `wasm_canvas_entry.cpp`, `wasm_canvas_support.cpp`,
`wasm_canvas_paint.cpp` and `webgpu_canvas_target.cpp` compile under the `wasm-webgpu` preset (verified
here with emsdk 7.0.0, producing all three `tessera_*.wasm`) and the host lanes type-check
`scene_frame.cpp`/`canvas_report.cpp`/`wasm_canvas_request.cpp` as OBJECT libraries, but a real WebGPU
device still needs a browser: `pages.yml` / `webgpu.yml` run the no-device contract, and **the pixel
comparison has not run anywhere yet** — see 8.3.

### 8.7 Files

| Path | Role |
|---|---|
| `tools/codegen/scene_prepare.{hpp,cpp}` | the shared stage sequence (`prepare_scene`) |
| `tools/codegen/scene_frame.{hpp,cpp}` | paint into an RHI target; `scene_frame_render` adds offscreen+readback |
| `tools/codegen/scene_frame_main.cpp` | `tessera_scene_frame`: renders a PNG (vulkan or null) |
| `tools/codegen/scene_frame_compare.cpp` | `tessera_scene_frame_compare`: the pixel comparison |
| `tools/codegen/canvas_report.{hpp,cpp}` | the JSON report the page parses (host-compilable) |
| `tools/codegen/wasm_canvas_request.{hpp,cpp}` | the ABI's argument parsing, host-compilable (#444) |
| `tools/codegen/wasm_canvas_paint.{hpp,cpp}` | clear/paint into the page's canvas texture (#444) |
| `tools/codegen/wasm_canvas_support.{hpp,cpp}` | `clear_canvas` and the pointer vocabulary (#390) |
| `tools/codegen/preview_text.{hpp,cpp}` | the text stack over caller-supplied font bytes (#387) |
| `tools/codegen/wasm_canvas_entry.cpp` | the browser ABI: device handover, frame, apply, draw, `set_font` |
| `src/rendering/backend/webgpu/src/webgpu_canvas_target.cpp` | an RHI target over a host-owned view |
| `designer/web/src/views/canvas-{session,pane,device,frame,font}.ts` | the Canvas view (ADR-054) |
| `designer/web/public/fonts/` | the one shipped face and its OFL text (#387) |
| `scripts/ci/scene_frame_parity.py`, `scene_frame_canvas_probe.mjs` | the gate: native reference, node contract, comparison |
| `tests/unit/scene_frame_{tests,text_tests}.cpp` | the native half, as one `scene_frame` ctest case |

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
common-ancestor click rule, focus and propagation are `src/ui/scene/`, not a preview copy; the callback
then runs the handler's **TSX preview body** through the evaluator, the same action list
`parity::run_parity` executes for a `click <automationId>` step. Consequently:

- a handler that is a native `bind::` binding with no `preview` body runs **no** actions, exactly as
  in the parity runner, because its body is the developer's C++;
- a handler that declares a payload parameter (`(e: ClickEvent) => ...`) **cannot** run: the
  evaluator has no record value to bind to the parameter. It is reported in the result document's
  `unrun` list and counted by the gate, never silently treated as "nothing happened";
- real handler execution in the browser is the ADR-046 web target's job (the same code path as
  native, the browser being one OS behind the HAL), not this slice's.

### 9.2 State kept across edits: the exact rule

`tools/codegen/scene_state.hpp` states it in full:

> A state cell is carried across an edit when, and only when, the new evaluation has a component with
> the same **component id** that declares a cell with the same **cell id** (`state::count`) and the
> same **IR type**. Everything else starts at its initializer.

- The component id is the salted hash of source path plus component name
  ([`automation-ids-v1.md`](../specs/automation-ids-v1.md) section 2.1), assigned by
  `ids::assign_identity`. It is a pure function of the text, so two separate evaluations of edited
  text are directly comparable. Inserting, deleting or reordering components cannot hand one
  component's state to another, and a moved file is a different component.
- The cell id, not a position, is the cell identity: reordering `useState` calls keeps every
  existing value; renaming a variable makes it a different cell. The IR type is part of the key
  because the generated C++ field's type comes from it, and carrying an `int` into a `float` cell
  would be a conversion the native build never performs.
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
V1 has **no** state snapshot or restore, and this slice does not add one.

### 9.3 The artifacts and the gate

| Side | Artifact | Invocation |
|---|---|---|
| native | `tessera_scene_preview` | `--input-json OUT --script SCRIPT --file FILE --salt SALT FILE.tsx` |
| Wasm | `tessera_scene_preview.js` | `tessera_input_json(component, script)` over a staged project + `tessera_free` |

The result document is `{"inputVersion":1,"initial":[...],"steps":[...]}`; each step carries `found`,
`dispatched`, the `ran` and `unrun` handler ids, and the state cells after it. Cells are the existing
`ui::StateCell` shape, with `kind` and `value` as text so a diff names the cell that moved and never
has to reason about JSON number formatting.

`scripts/ci/scene_input_parity.py` copies `scene_snapshot_parity.py`'s shape: one corpus, both sides
executed, **byte-identical** documents, absolute paths resolved before any `cwd` change, and an
anti-vacuity rule. For input the vacuity that matters is different: every case declares a **minimum
state-cell count and a minimum number of handlers that must run**, and any non-empty `unrun` fails, so
"both sides clicked nothing" cannot pass. `scripts/ci/scene_input_native.py` is the half that runs
without Emscripten, importing the gate's corpus and salt so the two cannot drift.

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

**CI-only, and why:** the Wasm half, for the same reason as section 7.4. Before #422 the authoring box had
no `$env:EMSDK`, so `tessera_input_json` was authored to the P1/P2 pattern and type-checked by the host
build but not executed. The byte comparison in `scene_input_parity.py` — and therefore P4's acceptance
sentence — is proven by the lane that runs the Wasm build.

## 10. P5: the sample gallery and the environment toggles (share links removed)

P5's acceptance was **"a compressed URL-fragment link round-trips; the samples from `samples/` load
within the project-load budget"**. The share link half was **removed** (owner decision, studio focus pass:
no cloud storage to share to; [`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 7.1), and so
was the bundled gallery (samples open from a clone); the rest of this section is the historical record.

### 10.1 The pieces

The page's chrome is **React 19 + TypeScript built by Vite** since issue #422 (ADR-054). What did
**not** move is the rule the round trip depends on: the codec (`designer/web/src/lib/share.js`) and
the state vocabulary (`preview-state.js`) are still plain ES modules with no DOM and no React,
imported directly from source by `scripts/ci/preview_share_roundtrip.mjs`, typed by their `.d.ts`
siblings rather than rewritten as TypeScript (a bundled copy is not the shipped source). The
imperative half lives in `designer/web/src/views/*-session.ts`: each session owns one Wasm module
instance, fills the childless leaves React committed, and calls
`import(wasmModuleUrl("tessera_<name>.js"))` with a `@vite-ignore`, so Vite neither bundles nor
preloads a module. [`studio-page.md`](studio-page.md) section 2 is the normative layout record.

### 10.2 What the toggles actually do

`viewport` and `dpr` are real layout inputs: `tessera_scene_json` passes them to
`ScenePreviewOptions`, so the scene page re-lays-out at the chosen rectangle. Measured with the native
CLI at 320×240 / 390×844 / 1280×800, the root node's bounds follow the request and the node count is
unchanged — the difference between "the toggle is wired" and "the toggle re-renders the same picture".

`theme` is carried in the link and persisted locally, but it is **not applied**: the null-RHI
snapshot path has no theme asset, so a themed preview would be a claim the module cannot keep — a
spec-level gap, recorded rather than papered over. A non-positive size means the
320×240 / DPR 1 default the parity gate pins (`tools/codegen/wasm_scene_entry.cpp`).

### 10.3 Verification and status

Verified on this Windows host (2026-11-15 for P5; re-run 2026-11-16 after the React conversion,
#422). The per-script case lists live in [`studio-page.md`](studio-page.md) section 5, which is their
normative record; here the summary is that every one of them passes against the built site:

- `preview_share_roundtrip.mjs` (19 cases, importing the codec **from source**),
  `preview_studio_smoke.mjs` (11, against `dist-ssr/` under a stub browser) and
  `preview_page_checks.mjs` (15, against the built page) all pass, as does
  `preview_browser_check.mjs` (11 browser checks against `build/pages`, including a real WebGPU
  frame).
- `python -B -m unittest discover -s scripts/tests`: 239 tests, OK; `check_source_size.py`: 0 errors.
- `node scripts/ci/preview_page_checks.mjs` gained the second textarea's two ids (issue #390) and
  `node scripts/ci/preview_browser_verify.mjs` is the 10-check browser gate; both pass here.

**The browser half.** The browser checks could not run on the authoring box before #422; they now do
(Chrome 143 with WebGPU, against `build/pages`), so the result above is recorded rather than deferred
to the deploy. A browser **without** WebGPU is now covered too: `preview_browser_canvas_handover.mjs`
removes `navigator.gpu` before the page loads and asserts the browser-side heading, and breaks the
module's handle table to assert the module-side one. One of these is a CI gate, not a developer-run
note: the device handover runs in the Pages lane with headless Chromium on SwiftShader (#375). A human
opening the deployed URL remains the last word on CSS. The three pages above are now one `index.html`
with three views (#392), and its chrome is React 19 + TypeScript built by Vite (#422, ADR-054);
[`studio-page.md`](studio-page.md) records both, with the budget re-records.
