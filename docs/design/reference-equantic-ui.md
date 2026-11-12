# Reference study: eQuantic.UI (lessons for Tessera)

Status: reference study (2026-11-12). Nothing here is normative; decisions stay in the ADRs and
specs linked below. The owner pointed at [eQuantic.UI](https://github.com/eQuantic/equantic-ui) as
"the kind of thing we are heading towards". This note records what that project does, how mature
it is, and what Tessera should copy, adapt or avoid, with a staged plan for a Tessera playground.

## 0. How to read this note

- **Subject.** eQuantic.UI is an MIT-licensed C# UI SDK. Components are written once in C#; on the
  web they are transpiled to JavaScript and server-rendered/hydrated, natively they run as .NET and
  are drawn by its own GPU engine, **Photon** (Metal and Vulkan), and a subset renders as email HTML.
- **Snapshot.** Shallow clone of `main` at commit `26ce351` (2026-11-12), its GitHub wiki and the
  GitHub API (issues, releases). The clone lived outside this repository; nothing from it was built
  or executed. No code or text is copied here: everything is summarized in our own words, and the
  file paths are citations into **their** repository (prefix `eq:`), not links.
- **Claim labels.** Every factual claim carries one label:
  - **[S]** verified from source: seen in their code, tests or config at `26ce351`.
  - **[R]** from their README, `docs/`, wiki or issue tracker (their own statement, not checked).
  - **[I]** inference by this study.
- Tessera terms used below: TSX front end, IR ([`ir-v1.md`](../specs/ir-v1.md)), IR evaluator
  (`src/compiler/eval`), designer ([`designer.md`](designer.md)), hosted studio
  ([ADR-045](../../decisions/ADR-045-browser-hosted-studio.md),
  [`designer-studio-v1.md`](../specs/designer-studio-v1.md)), web target
  ([ADR-046](../../decisions/ADR-046-web-target-for-demos.md)), app framework
  ([ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md)), backend extensibility
  (ADR-049, open as [PR #83](https://github.com/thnak/Tessera/pull/83)).

## 1. Architecture

### 1.1 Two compile pipelines from one component source

- **Web.** An MSBuild SDK (`eq:src/eQuantic.UI.Sdk/Sdk/Sdk.targets`) runs a `CompileEQuanticUI`
  target before Build, which invokes the transpiler CLI `eqc` (`eq:src/eQuantic.Build/Program.cs`)
  through a response file and a shell-free process launcher (`eq:src/eQuantic.UI.Sdk.Tasks/RunProgram.cs`)
  [S]. eqc reads the project with Roslyn's full semantic model, converts each syntax node through
  one of roughly 140 per-construct "strategies" (`eq:src/eQuantic.UI.Compiler/CodeGen/Strategies/`)
  into a small JS IR (`eq:.../CodeGen/Ir/`: expression, statement, class member, class, module)
  [S]. Exactly one writer per IR level prints it; operator precedence and parenthesization live only
  in the expression writer, a rule that came from a shipped syntax error [S]. Output is TypeScript
  bundled per page by an **embedded Bun** binary shipped as one NuGet package per OS/arch, so users
  need no Node.js [S][R].
- **Migration discipline.** Strategies not yet moved to the IR return raw text wrapped as an opaque
  node; a baseline file listing them may only shrink
  (`eq:tests/eQuantic.UI.Compiler.Tests/Coverage/ir-migration.baseline.txt`) [S].
- **Unsupported constructs fail the build.** When no strategy matches, the converter reports
  EQ1001/1002/1003 as errors and emits a placeholder (`eq:.../CodeGen/CSharpToJsConverter.cs`) [S].
  Codes are grouped (EQ1xxx translation gaps, EQ2xxx constructs that cannot cross to the browser or
  the client/server boundary, EQ3xxx generators, EQ4xxx SDK) and a test keeps the catalogue in
  `eq:docs/DIAGNOSTICS.md` in step with every reporting site [R].
- **Conformance harness.** `eq:tests/eQuantic.UI.Conformance.Tests` transpiles a C# expression or
  block, runs it against the real bundled runtime under Bun (or Node), evaluates the same code with
  Roslyn scripting, serializes both to canonical JSON and requires equality; a generated
  differential suite uses a seeded PRNG so failures name their seed [S]. The README claims "well
  over a thousand cases" [R]; that folder has 43 test attributes [S], so the case count must come
  from data-driven theories and generated programs [I].
- **Native.** The same components compile as ordinary .NET; Photon lays them out with its own C#
  flex engine and records a display list per frame [S]. No transpilation exists on this path [S].
- **Source generators** (`eq:src/eQuantic.UI.Generators`) emit one factory function per app
  component (JavaScript has no overloading, so the widest constructor wins), a hydration manifest,
  a CSP, and form plumbing [S].

### 1.2 Component model, state and reactivity

- Base types (`eq:src/eQuantic.UI.Primitives/Nodes/UiComponent.cs`) [S]:
  - `UiComponent` with an abstract `Build(ComponentContext)` that must be pure over state and
    context; `StatelessComponent`; `StatefulComponent` with `SetState(Action)`, which runs the
    mutation and marks only its own tree position dirty, and posts the mutation to the UI dispatcher
    when called off-thread.
  - Lifecycle: `OnMount`, `OnUnmount`, `OnDependenciesChanged`, and `AdoptConfig(next)` (Flutter's
    `didUpdateWidget`). The source comment admits a footgun: a retained instance keeps its first
    configuration unless `AdoptConfig` is overridden, and many shipped components had that bug [S].
- `VisualNode` is a **closed hierarchy** (private-protected constructor, sealed concrete nodes) of
  about 40 target-neutral nodes: layout (`Box`, `Row`, `Column`, `Grid`, `Stack`, `ScrollView`,
  `Overlay`...), content (`Text`, `TextEntry`, `Image`, `Canvas`, `WebFrame`, `CameraPreview`...),
  interaction and semantics (`Pressable`, `Link`, `Draggable`, `LiveRegion`...) [S]. Names avoid any
  target's vocabulary (`Pressable`, not "button"; "destination", not "href") [R].
- Each target is a **realizer**, a visitor over that closed set: web server lowering
  (`eq:src/eQuantic.UI.Web/WebLoweringVisitor.*.cs`), browser lowering
  (`eq:src/eQuantic.UI.Runtime/src/shared/lowering.ts`, exhaustive `switch` ending in
  `assertNever`), Photon emit and semantics visitors, layout measure visitor, and email (which
  refuses 33 nodes loudly) [S][R]. Their audit records that the earlier hand-written switches,
  several silent on unknown nodes, caused a cluster of defects before being replaced by visitors
  (`eq:docs/ARCHITECTURE-AUDIT.md`) [R].
- **Inherited state.** `InheritedComponent` with `context.DependOn<T>()` works only where a
  component has a tree position, which is Photon today; web and server return null until a tracked
  issue lands [S]. There are no observable/notifier primitives [R] (`eq:docs/FLUTTER-PARITY.md`).
- **Element tree.** Photon has a retained element tree with a build owner and dirty-only rebuild;
  the browser and server still use their own string-keyed identity schemes, four in total, all
  unstable under sibling insertion (`eq:docs/ELEMENT-TREE-PLAN.md`) [R]. Browser hot reload drops
  nested state for this reason [R].

### 1.3 Browser runtime, diffing and routing

- The TypeScript runtime (`eq:src/eQuantic.UI.Runtime`) has a keyed reconciler
  (`src/dom/reconciler.ts`): common head, common tail, then a key map plus a longest increasing
  subsequence to minimize moves [S]. That is the Vue 3/ivi algorithm [I].
- Hydration adopts the server DOM, except subtrees the server flagged as unmeasured, because the
  server has no fonts and cannot measure text [S].
- A nested `setState` re-lowers its whole page or island [R]; the scheduler races
  `requestAnimationFrame` against a timeout backstop [S].
- Router: `pushState`/`popstate`, guards before commit, prefetch, scroll restore; routes come from
  `[Page("/path")]` attributes and page modules are loaded lazily [S].
- **Pinned size budget.** A test gzips the served `runtime.js` and compares it with
  `eq:tests/eQuantic.UI.Server.Tests/Budgets/served-runtime.json` (319,580 bytes); it fails on
  more than 1% growth **or** more than 5% unrecorded shrink, and is regenerated only behind an
  environment variable [S]. The runtime includes the transpiled shared component library [R].

### 1.4 Styling and themes

- Styles are typed values, not CSS: `BoxStyle` is a record struct (sizes, padding, background
  token, radii, borders, shadows, transform, transition, per-state `Hover`/`Focus`/`Pressed` diffs)
  and `ColorToken` carries a light and a dark color (`eq:src/eQuantic.UI.Primitives/Theme/`) [S].
- Web: each declaration becomes one atomic class named from a stable hash; theme colors become CSS
  custom properties so rules are theme-independent; the C# atomizer and a byte-identical TypeScript
  twin hash identically so hydration never restyles (`eq:src/eQuantic.UI.Web/StyleAtomizer.cs`,
  `eq:src/eQuantic.UI.Runtime/src/shared/style-atomizer.ts`) [S]. A build-time hashing plan was
  abandoned for run-time hashing on both sides [R].
- Native: the same values resolve to pixels; no CSS exists [R]. `IAppTheme` gives semantic tokens,
  type roles, elevation and shape scales; a Material package derives a full light/dark scheme from
  one seed color [S].

## 2. Photon, the native engine

- **Two seams** (`eq:src/eQuantic.UI.Native.Engine/Rendering.cs`, `Rhi.cs`) [S]: a coarse
  backend interface (create surface, render a display list) and a thin RHI (device, render target,
  texture, command list). One shared encoder sits on the RHI, so Metal and Vulkan differ only in API
  calls. The RHI was extracted from a working Metal spike [S].
- **Fixed pipeline set** of six (SDF shapes, A8 texture, RGBA texture, layer composite, blur down,
  blur up); creating a pipeline during a draw is treated as a bug [S]. Every draw is a fullscreen
  triangle with a 160-byte uniform block, kept in a dynamic uniform ring on Vulkan because it
  exceeds the guaranteed push-constant size [S].
- **Display list.** A flat struct per command, nine kinds (clear, fill/stroke/shadow rounded rect,
  begin/end layer, texture, backdrop blur, annular sector); transform and clip are baked at record
  time; no general paths (fenced to "v2+") [S][R]. Shapes are signed distance functions; one C# file
  is the normative SDF math that both the CPU rasterizer and the shaders implement [S].
- **Redraw model.** Retained element tree, but each frame re-lays out and records a complete display
  list; rendering is on demand (caret blink schedules one future frame), with **no damage
  rectangles or partial redraw** [S].
- **Text.** The plan's HarfBuzz/FreeType/atlas path was reversed for each platform's own text
  engine (CoreText, DirectWrite + Direct2D, Android `StaticLayout`), so measurement and rasterization
  always agree on line breaks [R][S]. Each text block becomes one A8 texture; there is no glyph
  atlas [S]. Without a text service, text draws as deterministic bars for goldens [S].
- **Shaders.** One Slang source compiled offline to MSL/metallib and SPIR-V; outputs are committed;
  pipeline caches persist across launches keyed by a hash of the shader bytes [S].
- **Reference backend and goldens.** A scalar CPU rasterizer evaluates the same SDFs; it is the
  golden source (about 109 PNGs, tolerance 2 per channel) and the GPU backends are compared to a
  **live** reference render (max channel difference 4, at most 1% of pixels above 2)
  (`eq:tests/eQuantic.UI.Native.Engine.Tests/Golden/GpuParity.cs`) [S]. The CPU backend is also the
  production fallback on Windows and Android without Vulkan [S].
- **Allocation harness.** `PerfHarnessTests.cs` measures bytes allocated over 60 steady frames and
  fails above ceilings (about 42 KB/frame pooled, 200 commands, p95 33 ms) [S]. The stated goal of
  zero steady-state allocation is not met [S][R].
- **Shells and threads.** macOS, iOS, Android and Win32; all render on the platform main thread
  (run-loop timer, `CADisplayLink`, `Choreographer`, message wait with timer), not on the render
  thread the plan described [S][I]. IME through proxies on each platform [S]. Accessibility from one
  semantics tree computed from the realized frame, bridged to VoiceOver and TalkBack; Windows has no
  UI Automation provider and Linux has no shell [S][R].
- **Shared with web.** The component library, theme, vocabulary, semantics rules and focus/caret
  geometry are shared C#; layout is not: Photon has its own flex engine and the planned automated
  web/native layout comparison "never landed" (`eq:docs/SHARED-COMPONENTS-PLAN.md`) [R].
- The host class `PhotonHost` is about 3,500 lines combining frame driving, input, focus, IME,
  scroll and semantics; their own audit calls it the heaviest monolith [S][R].

## 3. Application-framework concerns

| Concern | What eQuantic does | Label |
|---|---|---|
| App object, lifecycle | `PhotonApplication.CreateBuilder(args)` wraps the .NET generic host (`eq:src/eQuantic.UI.Native.Hosting`); no resumed/paused lifecycle reaches components (listed as a gap) | [S][R] |
| Services/DI | Microsoft DI. Pages/roots use constructor injection; deeper components call `GetService<T>()`, resolved through an ambient `AsyncLocal` scope that returns null when absent (`eq:src/eQuantic.UI.Primitives/Contracts/CapabilityScope.cs`); in the browser, unresolvable services fail the build (EQ2116) | [S][R] |
| Device capabilities | About 18 typed interfaces (camera, location, biometrics, storage, secret store...) taken from DI; manifests and Info.plist are generated from constant builder calls read at compile time | [S][R] |
| Navigation | `[Page]` attribute routes, `Navigator.Of(context).Go(...)`; no native route stack, Android back dismisses the top layer | [S][R] |
| App state | `SetState`, `InheritedComponent` (Photon only), Server Actions and SSR prefetch for data; no observable store | [S][R] |
| Background work | off-thread `SetState` is queued to the next frame; frame ticker and clock interfaces; no isolate/compute equivalent | [S][R] |
| Server calls | `[ServerAction]` methods are an allowlisted RPC: origin check, content type, body cap, JSON depth limit, `[Authorize]` evaluated before the body (`eq:src/eQuantic.UI.Server/ServerActionsMiddleware.cs`) | [S] |
| Storage | `IAppStorage` and a separate `ISecretStore` (Keychain, Keystore, DPAPI files); the secret store is deliberately null on the web | [S] |
| Forms | `[FormModel]` on DataAnnotations classes generates a controller at build time because reflection cannot run in the browser | [S] |
| i18n | resx as source of truth; every translation's format arguments checked at build (EQ2100/2101); their plan says most of it is still design, plurals beyond two forms and RTL are out of scope | [R] |
| Auth | `[Authorize]`/`[AllowAnonymous]` on pages and actions; passkeys on the web first | [S][R] |
| Testing | headless test host project and golden infrastructure; cross-target semantics parity test with a shrink-only baseline | [S] |
| Native surfaces | **None.** `WebFrame` is a sandboxed iframe on the web and paints nothing on Photon; `CameraPreview` uploads CPU frames each frame; `Canvas` is limited to engine shapes; a test pins that no "PlatformView" concept exists | [S] |

## 4. Developer experience

- **Templates and SDK.** `dotnet new equantic-app` (web) and `equantic-native` (Photon), each with
  shell variants; the project file names only the SDK, and versions are pinned together [S][R].
  There is no separate CLI besides `eqc`, run through MSBuild and `dotnet watch` [S].
- **Hot reload.** Web: a server watches sources, re-runs the compile target, pushes events over
  server-sent events, and the page fully reloads and replays captured page fields through
  hydration; module hot-swap is deferred [S]. Native: a .NET metadata-update handler bumps a cache
  generation; state survives on position-kept instances; rude edits restart [S].
- **Edit-to-view is measured.** A script drives a real edit and stops the clock when the browser
  (via CDP) or the native window shows it; `eq:scripts/edit-to-view.budget.json` holds desktop to
  1 s and the web to a 37.7 s **ceiling** that may only fall (warm median 22.9 s against a 1 s goal)
  [S]. This honest, ratcheting budget is unusual and worth copying [I].
- **Source maps and error overlay.** Statement-level maps compose Bun's map with eqc's into JS to
  C#; a development overlay rewrites stack traces into C# frames [S][R].
- **Visual editor.** A VS Code extension talks to a long-lived design host over newline-delimited
  JSON on stdio (`eq:src/eQuantic.UI.Design/Protocol.cs`): compile, diagnose, inspect, set/unset a
  property, insert/move/remove/duplicate children, render native [S]. It compiles unsaved buffers;
  design mode stamps each node with a source span so a click selects source; the host only
  **computes** span edits and the editor applies them, so one undo reverts one gesture [S]. A native
  frame is rendered by the CPU reference rasterizer in a disposable child process killed after 10 s
  [S]. Measured warm compile p50 about 0.3 s (wiki "VisualEditor") [R].
- **Docs process.** Measured audits and plans in `eq:docs/`, a per-PR ledger folded at release,
  OpenSpec change proposals tied to issues, a 43-page wiki in five languages with machine-checked
  "since version" marks [R][S]. Their `CLAUDE.md` is over 900 lines and a `claude` contributor has
  commits [S]; the project is heavily AI-assisted [I].
- **Cadence.** About 20 GitHub releases `0.2.0-preview.49` to `.68`, roughly weekly, sometimes
  several a day; all packages publish to nuget.org together [R]. Breaking changes are allowed
  freely during preview, with no compatibility shims (`eq:docs/PRINCIPLES.md`) [R].

### 4.1 The playground

- Location: `https://ui.equantic.tech/playground`. Its site code is **not in the public repo**; a
  fixture names a separate repo (`equantic-ui-web`) that the GitHub API does not show [S][R].
- **Compiled on the server, run in the browser.** The site is itself an eQuantic app; Run and live
  diagnostics are ordinary Server Actions that call the compiler library [R][I]. The compiler has a
  dedicated **standalone mode** for it: one source file, no project references, no semantic model,
  emitting plain JavaScript that a browser imports through an import map with no bundler step
  (`eq:src/eQuantic.UI.Compiler/CodeGen/TypeScriptEmitter.cs`, `ComponentDefinition.cs`) [S].
  Because Roslyn's parse is lenient, the playground must ask for language errors first, otherwise
  bad code mounts and throws (`ComponentCompiler.GetLanguageErrors`) [S].
- Output mounts in a **sandboxed iframe** through the `WebFrame` node, whose sandbox grants only
  scripts by default (`eq:src/eQuantic.UI.Primitives/Nodes/WebFrame.cs`) [S].
- The editor is their own `CodeEditor` component with Roslyn completions and live squiggles
  [R]. The playground runs the deployed eqc, which can trail the newest preview (README) [R].
- **Costs visible in the code.** Many tests exist only because standalone mode cannot resolve
  symbols and needs heuristic fallbacks; several fidelity bugs were "compiled alone, as the
  playground compiles" [S][I]. No native/Photon preview, share links, examples gallery, limits or
  timeouts are documented anywhere public [R].

## 5. Quality and maturity

- **Tests** (attribute counts in source) [S]: native engine about 2,560, compiler 1,560, web 1,127,
  server 490, design 104, plus about 211 TypeScript specs. Playwright end-to-end suites exist but the
  CI workflow does not run them [S][I].
- **CI** (`eq:.github/workflows/ci.yml`) [S]: change classifier; Ubuntu and Windows test matrix;
  macOS for CoreText/Metal/AppKit, samples, packaging and an iOS simulator launch; no GPU runners
  (goldens from the CPU reference); public API files with the Roslyn analyzer; a package-surface
  gate that tags every shipped assembly as contract, tool or experimental.
- **Performance claims.**

| Claim | Status |
|---|---|
| Runtime gzip size budget | verified: test plus JSON budget [S] |
| Steady-frame allocation ceilings | verified: perf harness [S] |
| ListView with 10,000 rows draws a screenful | verified: test opens 10,000 rows [S] |
| Edit-to-view | verified and honest: desktop meets 1 s, web far over [S] |
| "Bun up to 10x faster than Node tools" (wiki "Performance") | unpinned marketing [R] |
| Hydration benchmarks, tree-shaking | not done, admitted in their roadmap [R] |

- **Maturity** [R]: created 2026-01-22, about 1,600 commits, effectively one or two authors with AI
  help, 21 stars, 337 open and 591 closed issues. The roadmap targets 1.0 as "web stable, native
  experimental".
- **Pain points from open issues** [R]: web edit-to-view of 23 s because eqc is not incremental
  (#900, #1277-#1281); the element-tree re-architecture before 1.0 (#1301, #1453, #1455); web vs
  Photon drift in adaptive thresholds (#1465) and accessibility semantics (#1328, #1330, #1333);
  transpiler fidelity bugs (#1464, #1483); generated-name collisions surfacing far from their cause
  (#1427); flaky CI (#1298, #1397, #1473).

## 6. Lessons for Tessera

Tessera differs on the axes that matter: production is native C++23 generated from TSX via IR
(no transpiled runtime and no .NET), the browser is a design-time studio
([ADR-045](../../decisions/ADR-045-browser-hosted-studio.md)) and a secondary demo target
([ADR-046](../../decisions/ADR-046-web-target-for-demos.md)) rendered by the same engine through
WebGPU, and hot paths forbid RTTI and hidden allocations. eQuantic's web side is a DOM realizer;
ours is a canvas, so its DOM-specific machinery (atomic CSS, hydration, SSR) has no direct analogue.

### 6.1 Copy

1. **Closed vocabulary, exhaustive visitors, no silent default.** Their defect cluster from silent
   switches is the cheapest lesson here [R]. Tessera's IR node and scene node kinds should be
   dispatched by exhaustive `switch` (with `-Wswitch-enum` style warnings as errors) or
   `std::visit`, never a `default:` that drops a node. This fits the no-RTTI rule.
2. **Unsupported constructs are build errors with stable, documented codes**, and a test keeps the
   code catalogue and the reporting sites in step. Tessera already rejects unsupported TSX in the
   front end ([`designer.md`](designer.md) section 2.1); add the catalogue-sync test.
3. **Differential conformance harness with seeded fuzzing.** Their JS-vs-.NET harness is our IR
   evaluator vs generated C++ (`tests/parity/`, preview-parity runner). Extend it with a seeded
   program generator whose failures print their seed.
4. **Shrink-only baselines and ratcheting budgets.** Gap lists, size budgets and allocation
   ceilings that fail on growth **and** on unrecorded improvement, regenerated only behind an
   explicit variable. Apply to the Wasm download budget
   ([`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 2.3), frame allocation counts
   (ADR "no hidden allocations"), and JSX construct-gap lists.
5. **Measure edit-to-view end to end** with a script and a budget file, for the studio (edit to
   repainted canvas) and native hot reload. ADR-045 already says size and startup are product
   concerns; edit latency belongs next to them.
6. **Normative reference rasterizer, live GPU parity.** Their GPU-vs-live-CPU comparison with a
   tolerance and an outlier fraction matches ADR-009 and ADR-046 item 11; adopt the
   "max delta plus at most N% of pixels" rule for WebGPU vs lavapipe instead of a single threshold.
7. **Editor applies edits; host only computes them.** Same split as our `source.patch` in
   [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md); their one-undo-per-gesture result
   confirms the choice.

### 6.2 Adapt

8. **Standalone compile mode for a playground, but without their fidelity tax.** eQuantic had to
   add a reference-free compile mode and heuristic fallbacks because Roslyn needs references for
   semantics. Tessera's TSX front end is self-contained and has no external symbol universe, so the
   playground can run the **same** front end, analyzer and IR lowering as the CLI, in the browser,
   with no special mode (see section 7). Keep that property deliberately: any feature that needs
   project-wide resolution must also work from the files the studio holds.
9. **Typed service access through DI** maps onto ADR-050's `useService<Interface>()`: eQuantic
   resolves through an ambient `AsyncLocal` that returns null when a service is missing [S], and
   its browser path fails the build for unresolvable services (EQ2116) [R]. Tessera's choice is
   stricter and better for hot paths: resolve once at mount into a typed pointer, and report missing
   providers at startup. Do not copy the ambient lookup or the null result.
10. **Capabilities declared in code, manifests generated.** Their generator reads constant builder
    calls to write Info.plist and Android manifests, rejecting non-constant arguments [S]. Tessera
    should do the same from `tessera.json` modules (ADR-050 section 1 rule 3): the permission
    module declares what it needs, packaging writes the platform files.
11. **Per-platform text engines** gave them parity of line breaks with each OS's look [R]. Tessera
    chose FreeType plus its own shaping for deterministic goldens; keep that, but note their
    chose FreeType rasterization and a bounded glyph atlas for deterministic goldens
    ([`text.md`](text.md)); keep that, but note their reason (measurement and raster must come from
    one source of line breaks) applies to our measurement seam and atlas path too.

### 6.3 Avoid

12. **Twin implementations kept in step by hand.** Their C#/TypeScript twins (vocabulary, lowering,
    style atomizer) are their top structural debt [R]. Tessera's preview runs the same C++ through
    Wasm (ADR-037, ADR-046), so keep it that way: no JavaScript re-implementation of any engine
    logic in the studio shell.
13. **Ambient global context** (a dozen `AsyncLocal` channels, process-wide statics) [R]; Tessera's
    explicit app-context pointer through mount (ADR-050 section 3) avoids it.
14. **A monolithic host class** combining frame, input, focus, IME, semantics [S][R]. Keep
    `AppHost`/`WindowHost`/`RuntimeLoop` split by responsibility (the 500-line file limit helps).
15. **Full redraw without damage** [S]: Tessera already has damage tracking and scissored redraw
    plans; keep "static UI idles with zero presents" as a gate.
16. **No native-surface concept.** eQuantic cannot embed video, maps or 3D on native [S]. Tessera's
    typed `NativeSurface` refs ([`native-render-surfaces.md`](native-render-surfaces.md)) are a real
    differentiator; ship them with a parity story for the web target (the surface renders through
    the WebGPU RHI there).
17. **Server-side compilation for the playground** (rejected by ADR-045 for privacy and cost).
    eQuantic's server compile is what lets it run Roslyn; Tessera does not need it (lesson 8).
18. **Retained-instance config footgun** (`AdoptConfig`): with ADR-040 developer classes, generated
    code must update props on the retained instance by default, never leave it to the developer.

### 6.4 Top 10 lessons (summary)

1. Exhaustive dispatch over a closed node set; never a silent default (1).
2. Stable, documented diagnostic codes with a catalogue-sync test (2).
3. Evaluator-vs-codegen differential tests with seeded fuzzing (3).
4. Shrink-only baselines and two-sided ratchets for size, allocations and gaps (4).
5. An edit-to-view budget measured by a script, for studio and native (5).
6. GPU parity as "max delta plus outlier fraction" against the software reference (6).
7. The playground compiles in the browser with the full front end; no special mode (8, 17).
8. Typed, resolve-at-mount services; no ambient or nullable lookup (9, 13).
9. Native surfaces are a differentiator; keep them first-class on native and web (16).
10. No hand-kept twins: one C++ engine everywhere, the JS shell holds no engine logic (12).

## 7. A Tessera playground

### 7.1 What the owner liked, and what Tessera can do differently

eQuantic's playground gives a URL where a person writes a component, presses Run and sees it [R].
Tessera's equivalent is the hosted studio of
[ADR-045](../../decisions/ADR-045-browser-hosted-studio.md) reduced to a minimum: a static page on
GitHub Pages where the whole pipeline (TSX front end, IR lowering, IR evaluator, style, layout,
text, batched renderer, WebGPU RHI) runs as one Wasm module. Compared with eQuantic [I]:

- **No server.** Nothing to host, abuse or rate-limit; code never leaves the browser
  (ADR-045 decision 1 and its rejected "compile on a server" alternative).
- **Same pixels as native.** The preview is the real engine through WebGPU, not a DOM
  approximation; eQuantic's playground shows only the web realizer.
- **No fidelity tax.** The evaluator runs IR, and the parity runner already holds it to generated
  C++; nothing like a reference-free compile mode is needed.
- **Trade-off.** Developer C++ (ADR-040 `bind::` handlers) cannot run in the playground; preview
  bodies and fakes stand in (designer section 2.1, ADR-050 section 3.1 item 6). A later "run the
  real app" mode is the web target of ADR-046, built offline, not per edit.

### 7.2 Existing pieces

| Piece | State today | Where |
|---|---|---|
| TSX lexer/parser/analyzer and IR lowering | implemented (native) | `src/compiler/{analyzer,lowering,ir}`, [`jsx-frontend.md`](jsx-frontend.md) |
| IR evaluator | implemented | `src/compiler/eval`, [`designer.md`](designer.md) section 2.1 |
| Evaluator vs codegen parity runner | implemented | `tests/parity/`, `samples/parity-demo` |
| Stylesheet engine, Yoga layout, text, batched renderer | implemented (native) | `src/ui/`, `src/rendering/gpu/` |
| Compiled stylesheet blob | implemented | [`compiled-stylesheet-v1.md`](../specs/compiled-stylesheet-v1.md) |
| WebGPU RHI backend | mapping tables and WGSL registry built; device half is an unbuilt skeleton | `src/rendering/backend/webgpu`, [`gpu/webgpu-backend.md`](gpu/webgpu-backend.md) |
| Headless automation harness | implemented (native, JSON lines) | `src/harness`, [`headless-automation-v1.md`](../specs/headless-automation-v1.md) |
| Designer protocol, studio spec | specified | [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md), [`designer-studio-v1.md`](../specs/designer-studio-v1.md) |
| Static web client pattern (Vite) | exists for the wiki | `tools/wiki` |
| Emscripten preset, `designer/web` | missing (`designer/web` is an empty placeholder) | ADR-045 consequences |
| Browser HAL (canvas, input, IME, clipboard) | missing | ADR-046 decision 3 |

### 7.3 What is missing

1. An Emscripten CMake preset and toolchain that builds the front end, evaluator and engine as one
   single-threaded Wasm module (ADR-045 decision 5), with fonts loaded as data.
2. The WebGPU device half running in a browser canvas (surface configure, present, resize).
3. A minimal browser HAL: pointer and keyboard events into `InputEvent`, canvas resize and DPR,
   visibility (shared with ADR-046 decision 3; the IME bridge can come later).
4. A C ABI for the Wasm module: `load(files)`, `update(path, text)`, `diagnostics()`, `frame()`,
   mapped onto designer-protocol messages so the playground is a subset of the studio, not a fork.
5. Preview bodies or fakes for `bind::` handlers and `useService<T>()` (ADR-050 section 3.1 item 6).
6. Sharing: project state encoded into the URL fragment (compressed) so links work without a
   server; larger projects export as zip (studio D-S4).
7. Budgets: download size, cold time-to-first-frame and edit-to-view, measured in CI (lessons 4, 5).

### 7.4 Staged slices

Each slice is independently shippable and ends with a test and a docs update. They refine the
studio slices D-S2 to D-S5 of [`designer-studio-v1.md`](../specs/designer-studio-v1.md) section 12
rather than adding a parallel plan.

| Slice | Work | Acceptance |
|---|---|---|
| P0 | Emscripten preset; front end, analyzer and IR lowering compiled to Wasm; Node smoke test that compiles `samples/parity-demo/Counter.tsx` and returns the same IR JSON as the native CLI | byte-identical IR on native and Wasm |
| P1 | Diagnostics-only page (studio D-S5 early form): static page, editor textarea, Run validates TSX and lists diagnostics with spans; deployed to GitHub Pages from `main` | deployed URL shows the build id; diagnostics match native |
| P2 | Evaluator + scene + layout in Wasm with the **null** RHI backend: the page shows a scene/IR tree and computed layout boxes as HTML (no GPU yet), click a node to see its source span | scene snapshot equals the native harness snapshot for the samples |
| P3 | WebGPU device half in the browser; canvas preview of the evaluated scene; render-on-demand via `requestAnimationFrame`; "unsupported browser" page | WebGPU frame vs lavapipe golden within the parity tolerance (lesson 6) |
| P4 | Interaction: browser HAL pointer/keyboard into the evaluator; handlers run; state persists across edits by component identity and key (designer section 4) | scripted clicks give the same state cells as the parity runner |
| P5 | Shareable links (compressed URL fragment), sample gallery from `samples/`, theme/DPR/viewport toggles | link round-trip test; samples load under the project-load budget |
| P6 | Budgets in CI: compressed download, cold first frame, edit-to-view; warnings first, then ratchets (ADR-045 consequence) | budget file with two-sided ratchet |
| P7 | Grow into the studio: picker overlay, style inspector and `source.patch` (designer section 8), skins (D-S3), MCP adapter (D-S6) | studio slices' own acceptance |

Dependencies: P0 and P1 need only the compiler and an Emscripten build, so the URL has value
before the WebGPU backend is complete (as the studio spec already allows). P3 depends on the
WebGPU backend's device half; P4 shares its input work with the ADR-046 web target.

### 7.5 Risks

- **Wasm size and cold start** (ADR-045 consequences): the front end, evaluator, ICU-free text and
  the engine in one module; measure at P0.
- **Single-threaded parse jank** on large files; component-granular reparse and a worker are already
  in the designer design.
- **WebGPU availability** varies by browser; the diagnostics page (P1) and scene view (P2) still
  work without it.
- **Expectation gap:** users will expect their C++ handlers to run; the page must label preview
  bodies clearly and point to the ADR-046 web target for full apps.

## 8. Sources

- Repository: <https://github.com/eQuantic/equantic-ui> at `26ce351` (MIT). Paths cited as `eq:...`.
- Wiki: <https://github.com/eQuantic/equantic-ui/wiki> (pages "Home", "VisualEditor", "Debug",
  "Performance").
- Their self-assessments: `eq:docs/ARCHITECTURE-AUDIT.md`, `eq:docs/FLUTTER-PARITY.md`,
  `eq:docs/ELEMENT-TREE-PLAN.md`, `eq:docs/NATIVE-GPU-ENGINE-PLAN.md`, `eq:docs/DESKTOP-PLAN.md`,
  `eq:docs/PROVING.md`, `eq:docs/PRINCIPLES.md`, `eq:ROADMAP.md`.
- Related Tessera documents: [`designer.md`](designer.md),
  [`shell-design-preview.md`](shell-design-preview.md),
  [`native-render-surfaces.md`](native-render-surfaces.md),
  [`service-resolve-benchmark.md`](service-resolve-benchmark.md),
  [`designer-protocol-v1.md`](../specs/designer-protocol-v1.md),
  [`designer-studio-v1.md`](../specs/designer-studio-v1.md).
