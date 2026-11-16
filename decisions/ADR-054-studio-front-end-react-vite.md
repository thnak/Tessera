# ADR-054: The studio front end is a React 19 + Vite application over the static Wasm host page

- Status: Accepted (2026-11-16, owner: "React 19 + Vite"; the studio is a real product — the place to
  design, review and test UI against mock services, not a preview demo)
- Date: 2026-11-16
- Related: [ADR-045](ADR-045-browser-hosted-studio.md) (browser-hosted studio; page as host, static
  Pages artifact, HTML/CSS studio chrome — its page-stack wording is amended here, its decisions
  stand), [ADR-037](ADR-037-self-contained-wasm-designer.md) (self-contained Wasm viewer),
  [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md) (own JSX/TSX front end),
  [ADR-024](ADR-024-native-first-boundaries.md) (no production JS/React runtime — unchanged),
  [ADR-050 section 3.1 item 6](ADR-050/service-provisioning.md) (designer preview fakes),
  [ADR-055](ADR-055-studio-mock-services.md) (design-time mock services, which resolves questions 2 and
  3 below), [tools/wiki/README.md](../tools/wiki/README.md) (in-repo React 19 + Vite precedent), GitHub
  issues #392, #391, #387-#390, #395, #408, #171, #375, #406

## Context

**The studio is the product's design-time workspace.** The owner direction (2026-11-16) is that it is
where a developer designs a screen, reviews it in the simulated device frame, and tests it against
**mock services** that simulate the app's service behaviour. That goal is larger than the preview
page the repository ships today: it needs a component model, shared state, panels, a file tree, an
editor, an inspector, a mock console and a status line, all kept usable while the Wasm viewer
reloads a frame.

**The current page does not have that model.** `designer/web` is 13 hand-written files plus one
generated sample manifest (`samples.json`), about 2,800 lines of HTML/CSS/JavaScript, organised as one
ES module per responsibility. Several files are already at or past the repository's soft file limit
before any of the follow-up work lands: `app.css` 466 lines, `canvas.js` 388, `scene.js` 339,
`studio.js` 326, `app.js` 295 ([CLAUDE.md](../CLAUDE.md): 300 soft, 500 hard; #392 cites the older
numbers `app.js` 329, `scene.js` 369, `canvas.js` 420 from the tree it was written against). #392
merges the three pages into one `index.html` and splits the modules; #391 adds the file tree, editor
and save; #171 adds the picker overlay, style inspector and skins; #408 adds fold simulation. A
mock-service console adds another pane. Nothing in the page gives those features a component or state
model; the only rule today is "one ES module per responsibility".

**What the documents say about the stack.** No document names a front-end framework or forbids one.
The constraints that exist are:

- [ADR-045](ADR-045-browser-hosted-studio.md) decision 3: the studio's own chrome is HTML/CSS, never
  Tessera-rendered, so a preview failure cannot take down the editor; the engine draws only the
  preview canvas. Its alternatives reject rendering the studio UI in Tessera itself for V1.
- [designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 2.1 (static site, "JS host
  shell", no SSR), section 3 ("four regions, all HTML/CSS"), section 10 (strict CSP: no inline
  script, no remote script; hashed assets under `/designer/v<major>/`).
- [compiler-wasm.md](../docs/design/compiler-wasm.md) section 10.1: the page pieces are pure ES
  modules, and `samples.json` is committed "so Pages needs no build step".
- #392's acceptance: one ES module per responsibility, files under the size limit, a re-recorded
  download budget.

Those are descriptive assumptions of the hand-written page, not a ban on a framework. The
repository's no-React rule is narrower: [ADR-024](ADR-024-native-first-boundaries.md) and AGENTS.md
non-negotiable 7 forbid a **JS/React runtime in a production app binary**; they say nothing about
design-time tooling. There is a working precedent in this repository: [tools/wiki](../tools/wiki/README.md)
is a static React 19 + Vite + TypeScript client (`react`/`react-dom` `^19.0.0`, `vite` `^7.0.0`,
`typescript` `^5.9.0`, `@vitejs/plugin-react` `^5.0.0`), built to a static `dist/`, and its README
states that Node.js is design-time tooling only. It is also missing from
[dependencies.md](../docs/specs/dependencies.md) and [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md)
— a documentation gap this ADR closes in the same landing.

**Why the decision cannot wait.** #392 is unstarted, has no open PR, and is the merge and
restructuring point of the page. Six items build on the shell it produces: #391 (folder IDE), #171
(picker/inspector/skins), #408 (foldable simulation), and #387-#390 (text, pointer input,
conditionals, author stylesheet). Deciding the stack after #392, or after #391, means writing the
shell twice and rebasing the rest onto the second version. Deciding now costs one conversion.

**Mock services are in scope, but they have almost no documentation.** The project folder itself is
documented ([tessera.json](../docs/specs/project-config-v1.md), ADR-014, plus
[designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 7 and #391), but its accepted-file
set has no TypeScript kind, so mock files would have to extend it. For the mock model a
repository-wide search finds one seed, [ADR-050 section 3.1 item 6](ADR-050/service-provisioning.md):
"The designer gets a fake per manifest interface that records calls; a project may register richer
preview fakes." The test-time equivalent exists ([ADR-050 section 3.1](ADR-050/service-provisioning.md)
tests provide fakes under the same interface ids; `FakeNet` in
[networking-v1.md](../docs/specs/networking-v1.md) section 4 is the deterministic scripted network
fake). No document defines the mock format, who executes a mock, how delays and failures are scripted,
how calls are recorded, or what the studio shows. The dependency `ui/services.json` is itself still
unspecced ([service-injection.md](../docs/design/services/service-injection.md) section 11.6 item 3:
"a spec-level artifact with no format in the repository yet"), and it has no reader in the tree
([security-review-v1.md](../docs/specs/security-review-v1.md)). So the product goal is real and the
design must make room for it, but its normative definition is a follow-up spec, not this ADR.

## Decision

1. **Stack.** The studio front end in `designer/web` is **React 19 + TypeScript, built by Vite**.
   Versions mirror the in-repo precedent unless the build needs a newer pin; `package-lock.json` is
   committed and pins the tree.
2. **The deployed artifact stays static.** Vite's output is assembled by the Pages lane into the
   existing static artifact; `dist/` is git-ignored and never committed. No server, no SSR, no remote
   scripts, hashed assets under `/designer/v<major>/`. [ADR-045](ADR-045-browser-hosted-studio.md)
   decisions 1-3 stand.
3. **React renders studio chrome only.** Toolbar, view switching, file tree, editor host, diagnostics
   panel, status line and the mock console are React; the engine still draws exactly one thing, the
   preview canvas in Wasm, and the page remains the host. The studio is never Tessera-rendered
   (ADR-045 decision 3's reason still holds: a preview crash must not take down the editor).
4. **One framework, no runtime styling engine.** React is the only UI framework in the page; no
   second framework and no CSS-in-JS runtime. Styles are static CSS (CSS modules or one stylesheet) so
   the production CSP stays "no inline script, no remote script".
5. **Design-time mock services are project files, held and compiled by the studio host.** A mock is
   a TypeScript file inside the project folder (a new accepted-file kind), owned and compiled by the
   studio chrome, keyed by a service interface id from the project's service manifest; it runs only in
   the studio, local-only, deterministic and bounded. The viewer observes a mock through the service
   seam (ADR-050 section 3.1 item 6's engine-side fake is the bridge), **never** by executing
   TypeScript in the engine. Running project TypeScript in the page is an explicit exception to
   ADR-045 decision 7 ("user code is IR only, never JavaScript") and needs its own ADR before
   implementation; the file kind, the in-browser compile/runtime dependency, the sandbox, the
   scripting (responses, latency on the simulation clock, failures), the recording, the caps and the
   protocol additions belong to that ADR and to `docs/specs/studio-mock-services-v1.md`.
6. **Build and gates move with the stack.** The Pages workflow builds `designer/web` before assembling
   the artifact; `scripts/ci/preview_page_checks.mjs` and `scripts/ci/preview_studio_smoke.mjs` check
   the built output; `PAGE_FILES` in `scripts/preview_budget_measure.py` measures the built page. The
   P6 two-sided ratchet is re-recorded in the same landing with the measured React bundle and an
   explanation in the PR (an unrecorded improvement fails the same as a regression).
7. **Dependencies and licensing.** React, ReactDOM and the build-time toolchain (Vite,
   `@vitejs/plugin-react`, TypeScript) get
   [dependencies.md](../docs/specs/dependencies.md) and
   [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) entries in the same landing, together with the
   missing `tools/wiki` entries. React remains a design-time dependency: it is never linked into a
   Tessera app binary or the Wasm engine, so [ADR-024](ADR-024-native-first-boundaries.md) is
   untouched.
8. **No accessibility or browser-floor change.** The studio chrome follows the repository's
   keyboard/focus/role rules (the file tree is `role="tree"` and keyboard operable, per #391); the
   WebGPU floor applies to the preview view, not to the chrome.
9. **Scope re-baseline.** #392 becomes the stack conversion plus the three-view merge: Vite scaffold,
   React shell, view modules, build/CI/budget. #391 and #387-#390, #395, #408 and #171 are
   re-baselined against the shell #392 lands; nothing in their acceptance changes except the files
   they edit. *As landed (2026-11-16, PR #420) #392 carried only the three-view merge and the module
   split; the conversion is its own slice — see the amendment note below.*

## Consequences

- **Positive.** The studio gets a component and state model for the tree/editor/inspector/console
  before they are written, instead of retrofitting one; React's ecosystem covers what a real studio
  needs (editor wrappers, virtualized lists, drag and drop, panels); the choice matches the in-repo
  `tools/wiki` precedent and reader expectations; mock services get a natural home in the host, where
  TypeScript already runs and where hot-reloading a mock cannot destabilise the Wasm viewer.
- **Costs.** The studio build and the contributor workflow gain Node/npm and Vite; the npm dependency
  tree is a new supply-chain surface (pinned lockfile, build-time only); the enforced download budget
  grows by the React bundle before subtracting the vanilla JS it replaces — the diagnostics Wasm is
  374,019 B gzip of the current 417,327 B total, so a React+ReactDOM delta of roughly 40-60 KB gzip is
  a small share of both that row and the 6 MB target in
  [designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 2.3, but it must be measured and
  recorded; the static page checks and `pages.yml` change; the docs that promise "no build step"
  ([designer-studio-v1.md](../docs/specs/designer-studio-v1.md) section 2.1,
  [compiler-wasm.md](../docs/design/compiler-wasm.md) section 10.1) are corrected in the follow-up docs
  slice.
- **Risks.** The React shell must not become a second app framework: it renders chrome, never user
  UI, and never runs project code. A framework upgrade is a normal dependency bump, not an
  architecture change. The `share.js` and `preview-state.js` modules should stay pure ES modules (no
  DOM, no React) so the Node tests keep testing the same bytes the page imports.
- **Mock services are not delivered by this ADR.** Until the mock ADR and spec land, a mock can only
  be the recording fake ADR-050 already names. Executing project TypeScript in the page needs an
  in-browser TypeScript compiler (a new dependency, or a declarative mock subset instead); that choice,
  the sandbox and the ADR-045 decision 7 exception belong to the mock ADR. Sequencing: the service
  manifest (`ui/services.json`) spec, then the mock ADR and spec, then the studio console; the React
  stack is what makes the console cheap.

## Alternatives

- **Keep vanilla ES modules and add an in-house DOM/state helper.** No build step, no new
  dependency, no budget or gate change. Rejected: the missing component/state model is the actual
  scaling blocker, and the page is already past the soft file limit before #391/#171/#408 add panes.
- **Preact, Lit or Solid + Vite.** Smaller bundles (Preact is roughly a tenth of React's gzip size).
  Rejected: React 19 matches the in-repo precedent and the component/editor ecosystem a studio
  needs; the gzip delta is small against the Wasm module and the 6 MB target.
- **React from a CDN through an import map, no build.** No bundler, but no TypeScript/JSX transform,
  no hashed asset names (ADR-045's versioned paths), and a remote script against the CSP. Rejected.
- **Render the studio in Tessera itself (dogfooding).** Rejected for V1 in ADR-045 (the engine being
  previewed must not be able to break its own editor, and it would need a second engine instance).
  Revisit with the owner-planned separate multi-threaded studio site, not in M4.
- **A separate studio repository or site.** Rejected for V1: the studio ships from this repository as
  one Pages artifact and reuses the same engine build; splitting it adds deploy and version-matching
  cost with no product need yet.
- **Put React in each of the three pages instead of merging.** Rejected: #392 already decided one
  shell; three React roots would reintroduce the duplication the merge removes.

## Amendment note

[ADR-045](ADR-045-browser-hosted-studio.md) is **not superseded**. Its decisions (static Pages site,
page as host, HTML/CSS chrome, no server, single-threaded Wasm) remain in force; this ADR amends only
the page-stack assumption that the host shell is hand-written ES modules with no build step.

## Amendment (2026-11-16): #392 landed without the conversion

#392 closed on 2026-11-16 in PR #420 as the three-view merge and the module split, in vanilla ES
modules: no Vite scaffold, no React shell, no re-recorded budget. Decision 9's assumption that the
conversion rides inside #392 therefore did not hold.

The conversion is now its own slice and lands first among the studio items, because #391, #171, #408
and #387-#390 edit the shell it produces; decision 9's ordering stands, only who carries the
conversion changes. Decisions 6 and 7 move with it: the Pages build step, the gate rewrite and the
re-recorded budget land with the code, and the dependency/licensing entries land in this docs landing.
ADR-045 is still amended, not superseded.

## Follow-up

1. **Docs slice:** update [designer-studio-v1.md](../docs/specs/designer-studio-v1.md) (stack, build,
   module table, a mock-service section stub),
   [compiler-wasm.md](../docs/design/compiler-wasm.md) section 10.1 (modules are React; the no-build
   wording goes), [designer.md](../docs/design/designer.md) (host stack),
   [dependencies.md](../docs/specs/dependencies.md) and
   [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md) (React/Vite plus the `tools/wiki` gap),
   [docs/README.md](../docs/README.md), and AGENTS.md Current focus / farm order.
2. **Code slice inside #392:** Vite scaffold, React shell and view modules; `pages.yml` build step;
   gate rewrite; budget re-record; keep the Wasm modules lazy and `share.js`/`preview-state.js` as pure
   ES modules.
3. **New spec and issue:** [ADR-055](ADR-055-studio-mock-services.md) records the mock model; its spec
   `docs/specs/studio-mock-services-v1.md` plus the designer-protocol additions cover the mock
   project-file kind (accepted files, `project.load`, limits, #391/#406 file policy), interface binding,
   response/latency/failure scripting on the simulation clock, call recording, caps, determinism, and
   the ADR-050 section 3.1 item 6 bridge. It depends on the `ui/services.json` spec, which does not
   exist yet.
4. **Browser lane:** the Playwright/WebGPU browser check remains an owner decision (#375); the React
   chrome is verified by the same lane when it exists.
5. **Re-baseline** #391, #387-#390, #395, #408 and #171 after #392 lands, then run the promotion sweep
   from AGENTS.md so the farm can pick them up.

## Open questions raised by this decision

These are not decided here. They are the questions the mock ADR, the mock spec and the project-IO items
must answer; each names what it would change. Question 2 and question 3 are resolved by
[ADR-055](ADR-055-studio-mock-services.md).

1. **Project container.** V1 defines a project as a folder with
   [`tessera.json`](../docs/specs/project-config-v1.md) at its root (ADR-014), plus zip export for
   download; there is no single-file project format. Does the studio need one for share/import beyond
   the 8 KiB share link and the zip? This changes the #391/#406 file policy and
   `designer-protocol-v1.md` `project.load`.
2. **Mock project-file kind.** Which extension and location (`mocks/*.mock.ts`?), which accepted-file
   class, which size/count limits, whether `project.load` carries it to the viewer at all (it should
   not), and whether the shared file-policy library (#391 for the page, #406 for MCP) owns the rule.
   The accepted-file list and the protocol's "project file" kinds are both extended by the answer.
   *Resolved by [ADR-055](ADR-055-studio-mock-services.md): `*.mock.ts`, 1 MiB each, 64 per project,
   host-only, never sent to the viewer.*
3. **Mock execution model.** Declarative data replayed by the engine-side fake (no user code, no
   ADR-045 decision 7 exception, no new dependency) or TypeScript compiled and run by the studio host
   in a sandboxed worker (in-browser TypeScript compiler, new dependency, explicit exception). If the
   latter: the sandbox API surface, the caps, and the bridge (host-supplied replay data versus a
   protocol round-trip per service call), and how both stay deterministic on the simulation clock.
   *Resolved by [ADR-055](ADR-055-studio-mock-services.md): a sandboxed studio worker with a lazy
   transpiler; no live synchronous round-trip; the bridge mode is part of the mock spec.*
4. **Logger alternative.** Should the studio also (or instead) expose a component-scoped logger —
   `ILogger<Component>`-style — for behaviour traces and test assertions? No such type exists today
   (only [`diagnostics::Logger`](../include/tessera/diagnostics/log.hpp), level/category/sink, no
   injection and no studio stream). It observes, it cannot control responses, so it complements rather
   than replaces mocks, and it needs service injection plus an IR/TSX surface.
5. **Services manifest sequencing.** `ui/services.json` has no spec and no reader in the tree
   ([service-injection.md](../docs/design/services/service-injection.md) section 11.6 item 3,
   [security-review-v1.md](../docs/specs/security-review-v1.md)). The mock work needs that manifest and
   its generated `services.d.ts` typings; does that spec land before the mock ADR, and who owns it?
6. **MCP counterpart.** Should an agent be able to drive the same mock states over MCP (#396), the way
   `set_viewport` drives the environment? If yes, mocks must be project data, not ephemeral studio
   state, which constrains question 2 and question 3.
