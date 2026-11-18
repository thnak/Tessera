# CSS isolation for components

Status: accepted for V1 (issue #463). Normative rules live in
[`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 3 and section 17, the import form in
[`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 4, the IR field in
[`ir-v1.md`](../specs/ir-v1.md) section 1, and the codes in
[`compiler-diagnostics-v1.md`](../specs/compiler-diagnostics-v1.md). This note records why the
mechanism is what it is; it does not restate the rules.

## The gap

Every stylesheet of an app enters one global cascade
([`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 7.2). A project isolates its CSS only by
naming discipline: the gallery prefixes its form classes with `f-`, its dashboard with its own
prefix. Two components, two teams, or a component and a library that pick the same class name
restyle each other, and nothing reports it.

What exists is not isolation. `@layer tessera-ui` orders the library's CSS below the app's, and
`::part()` ([`component-parts-v1.md`](../specs/component-parts-v1.md)) lets a host style a
component's named parts. Neither limits *where* a rule applies. There is no CSS-in-JS on purpose
([`designer-studio-v1.md`](../specs/designer-studio-v1.md)), so a TSX component has no way to own
its styles.

## The three candidates

**A. Compile-time class scoping (CSS Modules).** A CSS file imported as
`import styles from "./Card.module.css"` has its class selectors renamed to a stable,
collision-free name. `className={styles.title}` lowers to the renamed class. Zero runtime cost, the
renamed names are what the selector index sees, and the cascade is unchanged.

**B. Evaluate `@scope`.** A component's CSS is wrapped in `@scope (.card) to (.card-slot)`; the
engine matches a rule only inside the scope root and outside the limit. Standard CSS, one mechanism
for authors who already know it. Costs a selector-matcher change (scope root and limit as ancestor
checks, proximity as a cascade step between specificity and order per CSS Cascade 6), and it does
not stop a global rule from reaching in.

**C. Automatic per-component scope attribute.** The compiler gives each component's nodes a hidden
`data-t-<hash>` attribute and appends it to the component's own selectors (the Vue `scoped` model).
Cheap in the matcher, but it breaks for child component roots and slots and needs rules for both.

## Decision

**A for authoring, B as the later standard-CSS path. C is rejected.**

A wins on three counts. It matches the TSX import model the project already has, so a component
owns its styles the same way it owns its code. It needs nothing from the selector matcher — the
renamed names are ordinary class names, so the cascade, the selector index and the goldens keep
working unchanged. And it is zero-cost at run time, which matters because the paint path is the
one the perf budget guards.

C is the weakest fit: it breaks at child component roots and slots, and it needs a rule for both,
which is two rules more than A needs. B is the right answer for authors who write plain CSS and
should not be forced through a module file, but it does not stop a global rule from reaching in, so
it cannot be the isolation mechanism. It stays a separate, later issue.

A plain `import "./x.css"` keeps its current meaning: global, unchanged. Nothing existing moves,
which is why this needs no ADR — the change is additive and gated behind the `.module.css` suffix.

## Naming

The scoped class is `<local>_<h>`, where `h` is 6 lowercase hex digits of FNV-1a 64 over the file's
**project-relative path with `/` separators and its original case**. FNV-1a 64 is already the hash
of `IrModule::stylesheets` ([`ir-v1.md`](../specs/ir-v1.md) section 1), so the project gains no new
hash function.

It is a path hash, not a content hash, so editing a file never renames its classes. Moving a file
does rename them, which is accepted: a move is a deliberate act, and the alternative — a content
hash — would rename every class on every edit, which would churn every golden and every
click-to-source record for no benefit.

Path-only input makes the name identical on Windows, Linux and in the Wasm preview, because the
compiler library is the same one in both
([`jsx-subset-v1-jsx.md`](../specs/jsx-subset-v1-jsx.md), the front end links into both the native
CLI and the Wasm module). A backslash path is not accepted as input; the caller normalizes first.

Two files whose 24-bit hashes collide in one project are a compile error naming both
(`TESSERA_CSS_MODULE_HASH_COLLISION`), not a silent merge. 24 bits is small enough that a large
project could collide by birthday, and a silent merge would be a restyling bug that no test would
catch.

## What is never renamed

Part names, component names (type selectors), attribute names, ids, custom properties, `@layer`
names and `@font-face` families are never renamed. Only `.class` selectors are.

So `Button::part(label)` from a host sheet keeps working, and a module sheet may style `::part(x)`
of a child component. Custom properties (`--x`), `:root` and the theme asset stay global; a module
sheet reads and defines tokens as before. `@font-face` families stay global because V1 loads
committed fonts only.

`@keyframes` is the one non-class name that *is* scoped, because a keyframes name is a global
identifier in exactly the way a class name is, and two components both defining `pulse` would
otherwise collide. A `@keyframes` name inside a module sheet is scoped like a class, and
`animation-name` / the `animation` shorthand in the **same file** are rewritten to match. A
keyframes shared across files is defined in a plain `.css`.

## Cascade position

A module sheet is author origin and unlayered by default, and sits in the cascade by the
import-graph order of its file ([`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 7.2),
exactly like a plain sheet. Layer names are for ordering, not scope, so they stay global. Library
CSS keeps `@layer tessera-ui`, and unlayered module CSS beats it.

## Codegen

The compiler embeds each stylesheet as a compiled blob
([`compiled-stylesheet-v1.md`](../specs/compiled-stylesheet-v1.md)), and a module sheet's blob is
compiled from the **rewritten** text — the same scoping the lowering applies to `className` values,
so the page's class and the blob's selector are the same name. `StylesheetRef::hash` still hashes the
file's own bytes, because it identifies the source file: what is scoped is the text the blob compiles,
not the file the IR points at.

## Preview

The design-time preview compiles the **scanned** text when its stylesheet file is a module. The
studio files its sheet under a path like `ui/Preview.module.css`, so `preview_stylesheet_text()`
(`tools/codegen/scene_stylesheet.cpp`) scans it first and the compiled rules carry the same scoped
names the lowering already put on `className`. Without that, a tree full of `card_0b82a5` would be
matched against a sheet that only defines `.card`: nothing applies, no diagnostic is reported, and
the component is silently unstyled. The suffix decides, not whether the source imports the sheet —
the preview applies the author's CSS whether or not it is imported (issue #390), so an unimported
module sheet is scoped the same way `tessera_codegen` scopes the blob it embeds. Every other sheet
is compiled byte-for-byte as written, so no pre-existing preview, snapshot or golden moves.

A module sheet the scan rejects is reported as a `PreviewCssDiagnostic` carrying the scan's stable
`TESSERA_CSS_MODULE_*` code and message, and the preview compiles an empty sheet — not the raw
bytes (the silently-unstyled failure again) and not the built-in fallback (author CSS replaces it,
it is never resurrected for a broken sheet).

## `:global`

`:global(.x)` leaves one class unrenamed; `:global { ... }` is not supported in V1. One syntax, the
function form, as in CSS Modules. A block form would need a rule for what happens to the selectors
inside it that a function form does not, and no author needs it.

## Diagnostics

- `styles.x` with no `.x` in the file: **error**, naming file and class. This is deliberately
  unlike the unknown-class warning of a plain `className`
  ([`jsx-subset-v1-jsx.md`](../specs/jsx-subset-v1-jsx.md) section 10.3): a plain class name may be
  defined by any sheet in the project, so the compiler cannot know it is wrong, while a module
  class is defined by exactly one file the compiler has read.
- A class in a module file that no TSX in the project reads: **warning**
  (`TESSERA_CSS_MODULE_UNUSED_CLASS`).
- A malformed `:global(`: **error**. A `:global()` that matches nothing is not linted; matching is
  dynamic.
- `TESSERA_JSX_STYLES_DYNAMIC` for `styles[k]`, spread, or passing `styles` around — the same
  reason `style` keys must be static. A conditional is allowed when every read in it is a
  constant (`on ? styles.active : ""`): a class that follows state is common (a selected tab, an
  error field), the IR already has the `conditional` expression, and both branches are known
  classes, so substituting the scoped names and lowering the rest as an ordinary expression needs
  no run-time name lookup. The first slice refused it; the gallery form's segmented control showed
  that was too strict.
- `TESSERA_CSS_MODULE_HASH_COLLISION` as above.

## Inspector and tools

IR keeps both names per module class (`local`, `scoped`, `file`). The devtools tree, the studio
inspector and click-to-source show the authored `local` name with the file; the hash is only what
the selector index sees. An author who is told `card_0b82a5` has to reverse a hash to find their
own file, which is a worse experience than the collision it prevents.

The lookup is `ir::find_authored_class` / `ir::authored_classes`
(`src/compiler/ir/include/tessera/compiler/ir/authored_class.hpp`): given the IR's stylesheet
references and a node's `className`, it returns each class as the authored name plus the declaring
file, and keeps a global class as written. Scoped names are unique in a project (a hash collision is
a compile error), so the answer is never ambiguous. No inspector, devtools tree or click-to-source
exists in code yet ([`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md) section 5, and
[`designer-studio-v1.md`](../specs/designer-studio-v1.md) puts the native inspector out of scope);
the first one that does must show names through this lookup rather than print the hash.

## Out of scope

Composition (`composes`), a CSS preprocessor, scoping of `style=` (inline is already local), and
migrating `samples/gallery` beyond one page.

## Related

- Issue #463 (this decision), #390 (author stylesheet in the preview), #391 (open a project folder,
  multi-file preview).
- [`component-library.md`](component-library.md) — the library that ships CSS of its own.
- [`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 3 (non-goals) and section 17 (open
  questions).
