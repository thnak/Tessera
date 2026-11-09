# Machine-readable form of the intrinsic table

Part of [`jsx-intrinsics-v1`](README.md). This page is a proposal for owner approval [Q22]; no loader
or generator is written yet. It decides the form of the data, not its code.

## 1. Requirements

- One source for: the analysis pass (props, events, children, refs, constraints), lowering, the IR
  validator (it currently does not check intrinsic props, [`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md)
  "Validator coverage"), the Wasm designer evaluator, the runtime node factory, and the TypeScript
  typings generator (subset 16.2).
- The compiler has no JSON reader (jsx-ir-lowering decision 15 deferred it) and must not link the UI
  library; the Wasm module has no file system ([`jsx-subset-v1.md`](../jsx-subset-v1.md) section 15).
- No Node toolchain in the build ([ADR-036](../../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md)).
- Deterministic, no hidden allocation on the hot path of the runtime node factory.

## 2. Options

| Option | For | Against |
|---|---|---|
| A. JSON file embedded at build (the form chosen for the hook manifest, subset 7.1) | Editable data, readable by any tool | Needs a JSON reader in the compiler and the runtime first; parse cost and failure modes at start; a second schema to keep in sync with C++ enums |
| B. JSON source and a build-time generator that emits a C++ table | Data-first, no runtime parse | The generator needs a JSON reader anyway (a CMake-script generator is fragile for nested data); one more build tool before the parser stage exists |
| C. A hand-written `constexpr` C++ table in a leaf library, plus a small dump tool that emits JSON and `.d.ts` | No parser, no allocation, same bytes in native, Wasm and runtime; `static_assert` checks at compile time; unit tests and the layering test already exist for C++ | Changing the table means a C++ edit; non-C++ tools read the dumped JSON, not the source |

**Recommendation: C.** It is the smallest change that gives every consumer one source today, and the
JSON dump of section 4 is forward compatible: if a JSON reader is added later (decision 15), the dump
format becomes the input of option A or B without changing consumers.

## 3. Shape of the table (what the C++ source encodes)

Names are illustrative. The C++ types are plain aggregates of `std::string_view` and enums in
`constexpr` arrays; no virtual functions, no RTTI.

```json
{
  "schemaVersion": 1,
  "types": [
    { "name": "TextChangeEvent",
      "fields": [ { "name": "value", "type": "string" }, { "name": "target", "type": "TextTarget" } ],
      "primary": "value" }
  ],
  "common": [
    { "name": "key", "type": "string|int", "value": "expr", "lowersTo": "key" }
  ],
  "elements": [
    {
      "tag": "Button",
      "irKind": "Button",
      "children": "text+nodes",
      "refType": "Button",
      "impliedRole": "button",
      "acceptsClassName": true,
      "acceptsStyle": true,
      "props": [
        { "name": "label", "type": "string", "value": "expr", "required": false, "lowersTo": "props.label" },
        { "name": "disabled", "type": "bool", "value": "expr", "default": false, "lowersTo": "props.disabled" }
      ],
      "events": [
        { "name": "onClick", "payload": "ClickEvent" },
        { "name": "onKeyDown", "payload": "KeyEvent" }
      ],
      "constraints": [
        { "kind": "contentOneOf", "props": ["label"], "orChildren": true, "code": "TESSERA_JSX_BUTTON_CONTENT_CONFLICT" }
      ]
    }
  ],
  "aliases": [
    { "tag": "ul", "target": "View", "status": "mapped", "inject": { "role": "list" } },
    { "tag": "input", "target": "TextInput", "status": "mapped",
      "attrs": [ { "name": "autoComplete", "rename": "autofill" } ],
      "typeMap": { "password": { "secure": true }, "number": { "inputMode": "numeric" } } },
    { "tag": "table", "status": "unsupported", "alternative": "View with flex or grid style" }
  ],
  "domTypes": [ { "name": "HTMLDivElement", "target": "View" } ]
}
```

Field meanings follow the columns of README section 2. `children` is one of `none`, `text`, `nodes`,
`text+nodes`, `template`, `one`. `value` is `literal`, `expr`, `handler`, `element`. `lowersTo` is one of
`props.<n>`, `style.<n>`, `key`, `refs`, `events.<n>`, `automationId`, `children`, `node.<field>`.

Constraint kinds (a closed list, each maps to one generic diagnostic code): `requires` (prop needed),
`exclusive` (at most one of a set), `oneOf` (exactly one of a set), `contentOneOf` (a prop or the
children), `onlyOn` (prop restricted to a tag), `warnIfMissing` (for example value without onChange).
Rules that need types or the manifest (CustomElement props, `Image.src` file existence) are code in the
analysis pass, named in the `constraints` entry as `custom` with a code, so the table lists every rule
even when it does not evaluate it.

## 4. Location, owner, consumers

| Item | Proposal |
|---|---|
| Source of truth | `src/schema/intrinsics/` (a `constexpr` table in a header plus one `.cpp` for lookups), target `tessera_schema`; namespace `tessera::schema` |
| Dependencies | the C++ standard library only. It must not include UI, rendering, JSX or IR headers. The compiler front end, lowering, IR validator, designer and runtime all link it; this keeps "the compiler does not link the UI library" ([`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md) decision 10) true |
| Owner | the compiler front end owner for the structure; each element spec owner approves the rows of their element. A change to a row is a change to this spec, its tests and the dump together |
| New directory | `src/schema/` needs an entry in [`module-map.md`](../../architecture/module-map.md) and a layering test (it must not link anything of the project) |
| Dump tool | `tessera_schema_dump` (a C++ executable, no new dependency) writes `intrinsics.json` (sorted keys, two-space indent, the IR writer rules) and `tessera.d.ts` |
| Golden | `tests/golden/schema/intrinsics.json` is compared with the dump in a unit test; changing it is a deliberate update and must come with the spec change |
| Spec/table drift | the tables of this folder are the review authority. The dump test fails when the C++ table changes without the golden; the golden diff is reviewed against the spec text |

| Consumer | Uses |
|---|---|
| Analysis (`tessera_react_rules`) | `elements`, `common`, `aliases`, `types`: children rule, prop types and value classes, event payload types, ref element types, constraints |
| Lowering | `lowersTo`, defaults, the alias `inject` and `attrs`, the text-run rules of [`elements-basic.md`](elements-basic.md) section 0 (code, keyed by `children`) |
| IR validator | `irKind`, allowed `props`/`events` names per kind, `children` rule; closes the "not covered" gap of validator slice 2 |
| Wasm designer evaluator and runtime node factory | prop name to scene property, default values, record field paths for `e.target.value` |
| Typings generator | everything of `elements`, `types`, `aliases`, `domTypes` (section 5) |

## 5. Sharing with the runtime property registry and the typings generator

- **Style properties.** [`jsx-ir-lowering.md`](../../design/jsx-ir-lowering.md) decision 10 needs a
  "length vs unitless" table "from one source shared with the runtime property registry". That is a second
  table in the same `tessera_schema` library (`style` section: name, `kind` in `length`, `number`,
  `color`, `keyword`, `list`). `src/ui/style/property_registry.cpp` keeps initial values and
  inheritance (data the compiler must not need) and a unit test checks that every name of the schema
  style table exists in the registry and the reverse. The registry is not rewritten to consume the
  schema in the first slice.
- **Node props.** The runtime node factory looks up an IR prop name in the same element rows to apply
  it to the scene node, instead of its own copy of the names; unknown props cannot reach it because the
  validator rejected them.
- **Typings.** `tessera_schema_dump --dts` emits, per element, an interface of its props (`value` kinds
  `expr` as the TS type, `literal` as the literal type or union, `handler` as `(e: Payload) => void`,
  `element` as `JSX.Element`), the event payload interfaces from `types`, the type aliases `int` and
  `float` (subset 5), `JSX.IntrinsicElements` entries for the capitalized names and for every `mapped`
  alias tag with its rewritten attribute set (the HTML spelling, so `input` accepts `type` and
  `autoComplete`), the `HTML*Element` names of [`html-aliases.md`](html-aliases.md) section 4, and
  `ref` typed to the element. Unsupported tags are omitted so the editor underlines them. Per-project
  typings for components and `ui/elements.json` are generated by the same tool from their own inputs
  ([`custom-elements-v1.md`](../custom-elements-v1.md) section 4). The hook typings come from the hook
  manifest (subset 7.1) and are merged into one `tessera.d.ts`.
- **Hooks manifest.** It is a separate source today (JSON, needs a reader). If option C is accepted,
  the owner may want the same `constexpr` form for it; that is out of scope here and not decided.
- **Versioning.** `schemaVersion` follows [`versioning-v1.md`](../versioning-v1.md); adding an optional
  prop or event is additive; removing or retyping one is breaking.

## 6. What is not specified here

The loader, the generator, the exact C++ types, the CMake targets and the test names. They are the
implementation slice that follows approval of [Q22].
