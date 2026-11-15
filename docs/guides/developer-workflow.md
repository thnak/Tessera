# Developer workflow and generated-code ownership

Status: applies from Big Step C (compiler vertical slice) onward (see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). V1 below means the first-release scope defined there.

Two files per component, each with exactly one owner. The generator writes the base and is the only
writer of it; you write the developer class and the generator never touches it. There is no merge
step, no marker block and no middle ground: nothing is preserved across a regeneration because
nothing developer-owned is ever regenerated.

Normative decision: [ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)
(generated base plus developer-owned class), which supersedes
[ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) (protected regions and the
binding manifest).

## 1. Ownership rule

| Area | Owner | Rule |
|---|---|---|
| `<Component>.g.hpp` (generated base) | Compiler | Build output: regenerated wholesale, never edited, never committed |
| `<Component>.hpp` / `<Component>.cpp` (developer class) | Developer | Scaffolded once, then never rewritten |
| `src/main.cpp` and other hand-written files | Developer | Never rewritten by the generator |
| `tessera.json` | Developer | Validated; CLI flags do not rewrite it |
| `generated/automation.manifest.json` | Compiler | Generated; do not hand-edit |

The generated header declares `gen::<Component>Base<Self>`, a CRTP template that owns everything
derived from the TSX: state cells, refs, animated values, the node tree, `build`/`mount`/`update`/
`unmount`, `apply_state`, child sets, event adapters and inline setter handlers. Its data members
are `private`. Developer code reaches them through generated accessors: `x()` and
`set_x(value)` for a state cell, `ref_<name>()` for a ref, `<name>()` for a prop. The names come
from the TSX, so they survive any change to the generated storage.

The developer files declare the concrete class and nothing else generated:

```cpp
// Demo.hpp (developer-owned; the generator writes this only if it is missing)
#pragma once

#include "Demo.g.hpp"

namespace demo {

class Demo final : public gen::DemoBase<Demo> {
private:
    friend struct gen::DemoBase<Demo>;

    // bind::sync (Demo.tsx)
    void sync();

    // bind::sync_cleanup (Demo.tsx)
    void sync_cleanup();

    int sync_runs_ = 0;
};

} // namespace demo
```

## 2. Bindings are required members

Every `// Note: bind::<id>` in the TSX becomes a **required member** of the developer class:

| TSX | Required member |
|---|---|
| native handler `bind::<id>` with typed params, return `R` | `R <id>(params)` |
| `useEffect` with `bind::<id>` | `void <id>()` |
| effect cleanup (`bind::<id>_cleanup`) | `void <id>_cleanup()` |

Dispatch is static through the base, so a missing or mistyped member is a **compile error**, not a
warning and not a silently dropped region:

```text
error: static_assert failed: TESSERA_CG_MISSING_BINDING: bind::on_submit (ui/Form.tsx:42)
       requires `void on_submit(const SubmitEvent&)` in demo::Form
```

`TESSERA_CG_MISSING_BINDING` names the bind id, the expected signature and the TSX `file:line`. The
same check runs as a `static_assert` at the top of `build()`, so it fires when the component is
instantiated. Generated code also provides `gen::<Component>Contract<Self>` for IDE tooling.

The reverse case — a developer member whose `// bind::<id>` marker left the TSX — is
`TESSERA_CG_BINDING_UNUSED`, reported by the generator. It is a warning: the member stays compiled
but nothing dispatches to it. Delete it or restore the marker.

Renaming a bind id is therefore an ordinary reviewed edit: rename the marker in the TSX, rename the
member, rebuild. The compiler tells you if the two sides disagree; no migration tool and no
manifest editing are involved.

## 3. Adding a component

1. Create `ui/MyComponent.tsx`.
2. Use only allowed hooks: `useState`, `useEffect`, `useRef`, `useAnimatedValue` (V1).
3. Put `// Note: bind::<id>` next to logic that needs developer C++.
4. Run `tessera generate` (or the `tessera_codegen` CLI).
5. Fill in the scaffolded members in `MyComponent.hpp`/`.cpp`.
6. Run `tessera build` / tests.
7. Commit the TSX **and** `MyComponent.hpp`/`.cpp`. Never commit `MyComponent.g.hpp`.

Unsupported patterns are rejected loudly: custom hooks, context, suspense, dynamic component
types, and non-static conditionals/loops. Error boundaries (components that catch errors from their
subtree and show a fallback) are supported for isolating subtrees; do not use them as a catch-all
to hide bugs. See [`error-boundaries-v1.md`](../specs/error-boundaries-v1.md).

## 4. Regeneration

The generator is the codegen CLI (Big Step C):

```bash
# Regenerate the generated base into the build output directory and scaffold missing developer
# files. Existing developer files are never rewritten.
tessera_codegen --out build/generated --namespace demo ui/Screen.tsx

# Add --verbose to report each file as written or unchanged.
tessera_codegen --verbose --out build/generated --namespace demo ui/Screen.tsx

# CI gate: fail when the generated base is stale; writes nothing.
tessera_codegen --check --out build/generated --namespace demo ui/Screen.tsx

# Dev builds: `#line` directives map a C++ compiler error or debugger frame to the TSX statement it
# came from (issue #78, docs/specs/compiler-diagnostics-v1.md section 9). Off by default because it
# changes every generated byte; a release build and `--check` use the default.
tessera_codegen --write --line-directives --root . --out build/generated --namespace demo ui/Screen.tsx

# Numeric automation ids (docs/specs/automation-ids-v1.md): when a tessera.json is found at --root
# or above the input, its `app.namespace` is the id salt and its directory the project root, so
# every component and node gets a componentId/uiId and the scene node carries it as its automation
# id, and `automation.manifest.json` (ids to names, paths and sources) is written next to the generated
# files. A tessera.json without a usable `app.namespace` is an error. --salt overrides it.
tessera_codegen --salt factory --root . --out build/generated --namespace demo ui/Screen.tsx

# See tools/codegen and samples/compiler-demo (Demo.tsx + the developer-owned Demo.hpp/.cpp).
```

The CLI has two output modes and they are not a choice: `--write` (the default) writes, `--check`
compares and writes nothing. The CLI reads one or more `.tsx` files and runs the real front end
(lexer/parser → Tier-1 analysis → lowering → IR validation → codegen) in one process; diagnostics
are first-error-only and carry `file:line:column`. Exit codes are **0** ok, **1** diagnostics or
drift, **2** usage/setup.

What `--write` does per component:

- regenerates `<Component>.g.hpp` wholesale, with no merge and no diffing against a previous copy;
- writes `<Component>.hpp`/`.cpp` **only when they do not exist**, with one stub per bind id;
- never edits an existing developer file, even a stub it wrote itself;
- reports `TESSERA_CG_MISSING_BINDING` for a binding the developer file does not declare, with the
  stub to add as the suggested fix, and `TESSERA_CG_BINDING_UNUSED` for a `// bind::<id>` marker
  whose binding left the TSX;
- writes `automation.manifest.json` next to the generated files when id assignment is on.

A second run with unchanged inputs is a byte- and mtime-no-op, so build systems do not rebuild.
Files outside `--out` are never touched, and generated names are validated
(`component-name.g.hpp`; no path separators).

**`--check` reports drift of the generated base only.** Developer files cannot drift, because the
generator never writes them. A stale `--check` therefore means exactly one thing: the TSX or the
generator changed and `<Component>.g.hpp` was not regenerated.

A module that imports CSS also gets one `<module>_stylesheets.hpp` next to the component files: the
compiled style blobs as `constexpr` byte arrays plus the `kEmbeddedStylesheets` table
([`compiled-stylesheet-v1.md`](../specs/compiled-stylesheet-v1.md) section 9). It is generated
output with no developer half and is covered by `--check` like the bases. A stylesheet the compiler
cannot read, or whose string literal is unterminated, fails the run with a `file:line:column`
diagnostic before any C++ is written; a sheet without a blob keeps its
`TESSERA_CG_STYLESHEET_NOT_EMBEDDED` warning.

The output directory is build output: keep it out of version control. Commit only the `.tsx`
sources, the developer classes and the hand-written files. The samples follow that rule:
`samples/compiler-demo/Demo.hpp` and `Demo.cpp` are developer-owned and committed, while
`Demo.g.hpp` is generated into the build tree.

A controlled `TextInput`/`TextArea` owns its generated `EditController`/`TextInputRuntime`; bind
input events through the scene (do not call a controller directly) so committed edits emit
`TextChange` and the generated `onChange` setter updates state and repaints. A `value` prop must
produce a printable text value; an `onChange` binding must take a `TextChangeEvent` payload.

Generated components expose three lifecycle entry points: `build(parent)` attaches the tree,
`apply_state()` recomputes interpolated text (paint-only dirty marks), and `update()` calls
`update_effects()` then `apply_state()`. A manual `update()` is not needed after state changes: use
the generated `set_<state>(value)` setters (they schedule the component) and install an
`UpdateScheduler` on the `FramePipeline`, which flushes it at the start of each tick so effects run
and interpolated text is written in that frame. Call setters on the UI thread only (debug builds
assert); from a worker, post a completion and set state in its callback. A developer member that
writes a generated field directly cannot do so — the fields are `private`; go through the setter.
Generated effect bodies run inside `update_effects()`; mutate state cells there and `update()`
refreshes the text afterwards.

A component with a conditional child or a keyed list also gets generated child-set paths;
`apply_state()` runs them first. A conditional branch is built once and attached/detached at its
source position, so a child-set change marks the shared parent `Layout|Paint|Render` (never `Style`)
and an unchanged condition marks nothing. A keyed list has no item cap: its table
grows to the source, reuses nodes by key, and allocates nothing when the key list is unchanged. A list-typed state cell is authored like any other
state: `useState<string[]>(["a", "b"])` (literal elements) or a list reference, and a list-typed
prop is declared `list<T>` and mapped inside the callee. A component instance is allowed as a
conditional child and inside a list template (one instance per item; its state follows the item key
across a reorder); dynamic elements and record element types are not available yet (see
[`compiler.md` §11](../design/compiler.md#11-c-codegen-first-slice-implemented) and
[`compiler-node-state.md`](../design/compiler-node-state.md)).

### 4.1 Generated-UI headless demo

`tests/integration/generated_ui_tests.cpp` runs the generated sample UI for real, without a GPU or
window: it instantiates the developer class `demo::Demo`, calls the inherited `build()` into a
scene, lays the tree out with Yoga (`LayoutEngine` + `apply_layout`), paints it through `paint_scene`
and the deterministic CPU canvas, dispatches synthetic pointer/text input, and compares three
committed CPU goldens at tolerance 0.

```bash
# CTest (registered from tests/unit because tests/integration is not in the root build yet).
ctest -R generated_ui --output-on-failure

# Run directly; --write regenerates the committed goldens deliberately.
./build/<preset>/tests/unit/generated_ui_tests <font.ttf> tests/golden/baseline/generated_ui
./build/<preset>/tests/unit/generated_ui_tests --write <font.ttf> tests/golden/baseline/generated_ui
```

What it proves today: the generated `Button` `PointerUp` binding reaches the developer class and
increments the component's state cell, and the generated `apply_state()`/`update()` path repaints it
(`Count: 1`) marking `Paint|Render` but never `Layout`; the event route is the standard W3C
capture/target/bubble order; two runs are byte-identical; and the generated `TextInput` is editable
end to end through `tessera_text_edit` - the scripted typing/caret/selection sequence is published
as `PaintData::decoration`, and the `editing.png` golden shows the text, the selection highlight and
the caret (the demo asserts the caret/selection rects stay inside the content box, the caret sits at
the right edge of a half-open selection, and deleting the selection collapses it). Goldens:
`before.png`, `after.png`, `editing.png`; every frame paints with `FontPaintTextMetrics` so
decoration geometry matches the glyph renderer.

What it also documents as codegen gaps: controlled `value`/`onChange` wiring for `TextInput` (the
demo publishes the edit buffer itself; the generated binding still takes the generic `Event`) and
`automationId` emission (the demo resolves nodes by element kind). The precise controlled-input
contract the compiler must emit (event record fields/units, generated member signature, per-edit vs
commit timing, `PaintData`/state mapping) is in
[`compiler.md` §11](../design/compiler.md#11-c-codegen-first-slice-implemented).

## 5. Editing rules

- Never edit `<Component>.g.hpp`; it is regenerated and your edit is silently lost. Put the change
  in the TSX or in the developer class.
- Never add generated members to the developer class by hand; declare only the bind members and
  your own state/helpers.
- Keep the developer class the only place business logic lives; the TSX describes structure and
  preview behavior only.
- Treat a `TESSERA_CG_MISSING_BINDING` as a build break, not as advice: add the member with the
  signature the message prints.
- Resolve `TESSERA_CG_BINDING_UNUSED` explicitly — delete the stale member or restore the marker.
- Do not rely on generated line numbers or member names; use the accessors, which are named after
  the TSX and are stable.

## 6. Review checklist

- [ ] No edits under the generated output directory.
- [ ] `tessera generate --check` passes (generated base only).
- [ ] Every `bind::<id>` marker has a matching developer member; no `TESSERA_CG_MISSING_BINDING`
      and no `TESSERA_CG_BINDING_UNUSED` in the build log.
- [ ] The developer class compiles with warnings-as-errors on Windows and Linux.
- [ ] Tests cover the new behavior; no wall clock/network/system fonts.
- [ ] Docs updated for new config/behavior/ADR.

## 7. Contributing to the framework

- Read [`AGENTS.md`](../../AGENTS.md) and the relevant ADR (see [decisions/README.md](../../decisions/README.md)) before changing architecture.
- Every subsystem gets a design note in `docs/design/` and, if contentious, an ADR.
- New dependencies require `docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md` entries.
- Keep files under the 300/500-line soft/hard limits ([ADR-015](../../decisions/ADR-015-code-style-build-cache.md), code style and build cache).
- Run format/lint/tests on Windows and Linux (AGENTS.md definition of done requires both).
- Do not add a blocking wait on the UI thread; use async/cancellation ([ADR-021](../../decisions/ADR-021-async-runtime.md), async runtime).
- Do not leak OS/GPU types into the UI layer ([ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md), window resize and lifecycle; [ADR-023](../../decisions/ADR-023-os-event-normalization-and-foldables.md), OS event normalization).

## 8. Related documents

- [`project-generator.md`](project-generator.md) — CLI and generated project layout
- [`project-config-v1.md`](../specs/project-config-v1.md) — `tessera.json`
- [`ir-v1.md`](../specs/ir-v1.md) — IR contract
- [`compiler.md`](../design/compiler.md) — compiler pipeline
- [`code-style.md`](code-style.md) — C++ style and build cache
- [`testing.md`](testing.md) — test commands and conventions
