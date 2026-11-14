# God type audit v1

Status: analysis (docs-only, 2026-11-14). No code, spec or ADR changes here. This audit lists the
types in `src/` and `include/` that carry too many responsibilities, the measurements behind that
claim, and a proposed split for each in the structural patterns the project already uses (extract
collaborator, table-driven dispatch, tagged variant, facade). It applies the file-size and
one-primary-type rules of [code-style.md](../guides/code-style.md).

Terms: **god type** = one type whose members serve more than one reason to change, so a change to
one feature forces edits to a header every other feature includes. **Fan-in** = number of
translation units that name the type.

## 1. Method

Measured at commit `b2978b48` (2026-11-14, `origin/main`) over the 907 hand-written `.hpp`/`.cpp`
files under `src/` and `include/` (third-party, build output, tests and the build-generated
`src/compiler/lowering/generated/` directory excluded): logical line counts, member functions and
data members per type, `case` labels per enum type and per production TU (tests excluded), `#include`
fan-in split production/test, and the longest
single function per file. A member function is any member declaration including constructors and
destructors but excluding deleted copy/move members; a data member is a non-static member variable;
the longest-function figure spans the signature line through the closing brace. Files over the
[code-style.md](../guides/code-style.md#files-and-size) limits: 103 of 907 over the soft limit of
300 lines, 16 over the hard limit of 500.

The 16 hard-limit files are the god-class surface ranked below. Being over the line is not by
itself a god type (a literal table may legitimately be long); a type enters the list only when its
API surface also mixes responsibilities. The order is fan-in x unrelated responsibilities x cost of
a wrong split; re-check the ordering argument, not the digits, when a candidate is taken up.

## 2. Ranked summary

| Rank | Type | Header | Surface | Responsibilities | Primary pattern |
|---|---|---|---|---|---|
| 1 | `ui::Node` | [node.hpp](../../include/tessera/ui/scene/node.hpp) | 652 lines, 129 methods, 41 setters, 38 fields, 10 nested types | tree, geometry, scroll, interaction flags, metadata, aria, inline style, event bindings, paint payload, dirty propagation | role interfaces over one storage |
| 2 | `app::WindowHost::Impl` | [window_impl.hpp](../../src/app/window_impl.hpp) | 255 lines, 32 methods, 42 fields, 30 definitions over 3 TUs | presentation, adaptive scale, GPU timing, device recovery, fullscreen, cursor shape, caret, pause, pacing | facade + collaborator extraction |
| 3 | `ui::virtualization::VirtualRange` | [virtual_range.hpp](../../include/tessera/ui/virtual/virtual_range.hpp) | 292 lines, 46 methods, 13 fields, 3 enums, 4 public structs | geometry, key index, recycler, item state, focus, selection, anchor, node placement | composition of the collaborators it already owns |
| 4 | `app::AppHost` + `Impl` | [host.cpp](../../src/app/host.cpp) | 42 method definitions, 651-line TU | style tree, layout pass, overlays, adaptive quality, diagnostics, resize, embedded sheets | facade + phase objects |
| 5 | `rendering::gpu::Batched2DRenderer` | [batched2d_renderer.hpp](../../include/tessera/rendering/gpu/batched2d_renderer.hpp) | 271 lines, 42 methods, 36 fields | pass lifecycle, offscreen pool, glass composite, clip interning, scissor, transform, batching, vertex memory | state cluster + collaborators |
| 6 | `ui::frame::FramePipeline` | [frame_pipeline.hpp](../../include/tessera/ui/frame_pipeline.hpp) | 281 lines, 26 methods, 21 fields | tick order, interpolation, style bridge, springs, paint cache, damage decision | extract interpolation + bridge |
| 7 | `ui::text::EditController` | [edit_controller.hpp](../../include/tessera/ui/text/edit_controller.hpp) | 202 lines, 38 methods, 13 fields | buffer edits, key policy, IME, clipboard, history, caret policy, decoration | extract keymap + composition |
| 8 | `jsx::Parser` | [parser_internal.hpp](../../src/compiler/analyzer/src/parser_internal.hpp) | 191 lines, 78 methods over 11 TUs | expression, primary, type, module, component, JSX and bind-pairing grammars | split the declaration block by grammar |
| 9 | `runtime::FrameDiagnostics` | [diagnostics.hpp](../../include/tessera/runtime/diagnostics.hpp) | 167 lines, 17 counters, 20 accessors | frame counters, adaptive-quality state, frame-time source | split by subject |
| 10 | `ui::scene::InputRouter` | [input_dispatch.hpp](../../include/tessera/ui/scene/input_dispatch.hpp) | 26 methods, 11 fields | capture, hover chain, click synthesis, wheel, text routing | per-family dispatchers |

God **enums and payload structs** are a separate list in section 4: `a11y::Role` with four parallel
switches, `scene::Event` and `input::InputEvent`, `ir::NodeKind` with `ir::Node`, and three competing
JSON error enums.

## 3. Classes

### 3.1 `ui::Node` (rank 1)

**Evidence.** [node.hpp](../../include/tessera/ui/scene/node.hpp) is the largest header in the tree
(652 lines) and the most included one: 45 production and 117 test TUs include it directly. It
declares 129 member functions (41 setters, 12 `clear_*`), 38 data
members, 10 nested types, and the file also holds five enums. The out-of-line halves already had to
grow to stay near the rules: `node.cpp` (483 lines) and `node_metadata.cpp` (314 lines). The
responsibilities are only loosely related:

1. identity and tree structure (`add_child`, `insert_child`, `remove_child`, `parent`);
2. box geometry and transform (`bounds`, `span_bounds`, `transform`, `world_*`, `content_transform`);
3. scroll runtime (`scrollable_*`, `scrollport_*`, `set_scroll_offset`, smooth-scroll target,
   `ScrollBehavior`/`Overscroll*` inputs);
4. interaction policy (`visible`, `style_hidden`, `pointer_events`, `user_select`, `focusable`,
   `tab_index`, `auto_focus`, `focus_trap`, `typeahead`, `focus_group`, pseudo state);
5. element metadata for selector matching (`element_kind`, `element_id`, `class_name`, `part`,
   `component_name`, `role`, `automation_id`, `item_key`, `aria_*`);
6. inline style declarations; 7. generated event bindings; 8. paint payload and glass blur;
9. dirty-domain bookkeeping (`mark_dirty`, `mark_subtree_dirty`, `subtree_dirty`).

Each is a different owner in the module map, yet all nine change this one header, so a feature edit
recompiles 162 TUs.

**Hard constraint.** The split must not grow the object: `kNodeSizeBudget` = 320 bytes is asserted
at compile time in `node.hpp` and enforced by [node_size_tests.cpp](../../tests/unit/node_size_tests.cpp),
which also proves a plain node allocates only itself and that the rare payload tables allocate once
and are reused. The budget history (320 -> 336 -> 320 on #235 and #285) is recorded in
[performance.md](../design/performance.md). Any proposal that adds a vtable, a child object, or one
pointer per feature is rejected up front.

**Proposal: role interfaces over one storage.**

- `scene::NodeCore` — id, parent/children, dirty domains.
- `scene::NodeGeometryView` — bounds, span bounds, transform, `world_*`, `content_transform`, in
  `node_geometry.cpp`.
- `scene::NodeScrollView` — the scroll section including the packed `scroll_bits_` accessors, in
  `node_scroll.cpp`.
- `scene::NodeInteractionView` — visibility, hit, focus and selection flags, in
  `node_interaction.cpp`.
- `scene::NodeMetadataView` — kind/id/class/part/component/role/automation id, aria pairs, inline
  style, event bindings, paint payload; bodies already live in `node_metadata.cpp`.
- `scene::SceneExtras` (optional, later) — one side table keyed by `NodeId` owning the five lazily
  allocated payloads. Only attempt this with a measured win: the current design is one pointer and a
  load, a hash lookup is not obviously better.

`Node` keeps inline forwarders during migration, so no caller changes in the first slice.

**Slices.** (1) Move the responsibility blocks into `node_geometry.cpp`/`node_scroll.cpp`/
`node_interaction.cpp`, header unchanged. (2) Introduce the scroll and interaction views and move
the `src/ui/scene`, `src/ui/paint`, `src/ui/layout` call sites. (3) Introduce the metadata view and
move `style/`, `a11y/` and codegen call sites. (4) Delete the forwarders; re-run `node_size_tests`.

**Risks.** The size assert and the allocation-count test are the guardrails; run both on every
slice. Hit testing is hot: keep `hit_test_deep` an inline body on `Node` after the split, or the
mouse-move path pays a call it did not pay before.

### 3.2 `app::WindowHost::Impl` (rank 2)

[window_impl.hpp](../../src/app/window_impl.hpp) is 255 lines and declares 32 member functions and 42
data members; 30 definitions span `window.cpp` (692 lines), `window_lifecycle.cpp` and
`window_recovery.cpp`, with `window_config.cpp` holding the config mapping (no `Impl` definitions) and
`window_cursor.cpp` the pointer-shape seam. `src/app` is 2588 lines of TUs in total. It owns
presentation, the two-frame-slot gate, adaptive render-scale targets, the GPU frame-time probe,
device-loss recovery, fullscreen requests, caret blink, pause lifecycle and the pacer. The class is a
legitimate *owner* of those objects; the problem is that it also implements every policy over them,
and recovery and adaptive scale have nothing to do with each other beyond adjacency in the header.

**Proposal.** Keep `Impl` as the facade and the run loop, move four policies behind types:
`app::Presentation` (device, swapchain, pipelines, renderer, sink, slot gate, `recreate`/
`replace_device`), `app::ScaledTargets` (`ensure_scaled_target`), `detail::FrameTimeProbe`
(`pending_gpu_frame_time_ms`, `take_gpu_frame_time_ms`, `observe_frame_time`) and
`app::RecoveryFlow` (`service_device_loss`, `service_recovery`, `RecoveryPolicy`,
`FrameFailureGuard`). Start with `FrameTimeProbe` and `ScaledTargets`: pure state plus a few methods,
no lifecycle coupling. `Presentation` is the larger slice and follows the existing
`recreate_swapchain`/`replace_device` tests.

### 3.3 `ui::virtualization::VirtualRange` (rank 3)

292-line header, 46 methods, 13 members, three enums and four public structs, implemented in
four TUs (`virtual_range.cpp` 273, `_focus.cpp` 171, `_query.cpp` 102, plus geometry/recycler/state).
It is documented as "one virtualized collection", a legitimate aggregate, but it exposes all eight
collaborators' APIs and adds two policy layers on top.

**Proposal: composition, not inheritance.** `VirtualRange` keeps `update`, `sync`, `scroll_to_*` and
`apply_measure`. Extract `FocusCursor` (focus key plus `FocusStep`/`ScrollAlignment` policy — the
content of `virtual_range_focus.cpp` already), `SelectionSet` (the `ItemStateStore` wrapper) and
`RealizedWindow` (`realized_`, placement, container content size, `realized_node`/`realized_index_at`).
The result structs stay in the header: they are the boundary value type, not state.

**Note.** `ItemStateStore`, `KeyIndex`, `NodeRecycler` and `ItemGeometry` are already correctly
sized and single-purpose. The defect is that `VirtualRange` re-exports them (`item_state()`,
`geometry()`, `recycler()`, `anchor()`), making every caller depend on the aggregate. Add direct
accessors to the collaborators first, then delete the re-exports.

### 3.4 `app::AppHost` and `Impl` (rank 4)

[host.cpp](../../src/app/host.cpp) is 651 lines with 42 method definitions and 12 file-local helpers;
`host_theme.cpp` and `frame_capture.cpp` add more. It handles style tree and cascade rebuild, the
layout pass, safe areas and root size, overlay input and viewport, embedded/theme stylesheet
registration, adaptive quality, frame diagnostics, resize, and the `begin_frame`/`paint`/`end_frame`
schedule. The class is not over the size limit; the TU is the symptom.

**Proposal.** `AppHost` stays the public facade. Inside, extract `detail::StyleStage`
(`rebuild_style_tree`, `rebuild_cascade`, `apply_style_generation`, `register_embedded_stylesheets`,
`apply_root_size`), `detail::LayoutStage` (`run_layout_pass` and the observation rounds),
`detail::OverlayStage` (`overlay_viewport`, `handle_overlay_input`) and `detail::QualityStage`
(`observe_frame_time_*`, `quality_controller_config`). `begin_frame` then reads as the four stages in
order, which is what the frame-pipeline design note already describes.

### 3.5 `rendering::gpu::Batched2DRenderer` (rank 5)

271-line header, 42 methods, 36 members, three private nested structs (`ClipState`, `Batch`,
`FrameSlot`), implemented in `batched2d_renderer.cpp` (475 lines) plus `batched2d_clip_state.cpp`,
`batched2d_glass.cpp`, `batched2d_seam.cpp` and `offscreen_target_pool.cpp`. Responsibilities: frame
and pass lifecycle, the break/resume seam, the offscreen pool, the glass composite, the rounded-clip
stack and interning, scissor and transform stacks, batch splitting, per-frame vertex buffers,
content scale, counters.

**Proposal.** Three of these already have their own files and only lack a type: `gpu::ClipStateTable`
(the whole of `batched2d_clip_state.cpp`), `gpu::GlassCompositor` (`batched2d_glass.cpp` plus the
pool handle) and `gpu::VertexArena` (`slots_`, `slot_index_`, `buffer_used_`, `vertices_`,
`ensure_buffer_space`, `current_buffer`, `select_next_slot`). The renderer keeps the pass lifecycle,
the transform/scissor stacks and the emit calls; its header drops to roughly 150 lines. The seam
stays on the renderer because it must flush mid-frame state.

### 3.6 `ui::frame::FramePipeline` (rank 6)

281-line header, 26 methods, 21 members; `frame_pipeline.cpp` is 452 lines, `interpolation.cpp` 297,
`paint_view.cpp` 60. It owns the tick order (scheduler flush, animations, springs, style sync,
layout, damage), interpolation and duplicate-present suppression, the computed-style -> transition/
animation bridge, the paint cache, the redraw decision and the keyframe table.

**Proposal.** The two halves that already have files should have types:
`frame::InterpolationBuffer` (`previous_samples_`, `current_samples_`, `last_damaged_corners_`,
`alpha_`, `interpolate_`, `interpolation_view`) and `frame::StyleStage` (`style_bridge_state_`,
`style_bridge_snapshot_`, `style_keyframes_`, `sync_style`). `FramePipeline` keeps `tick`,
`finish_frame`, `paint_frame` and the cache. Both extractions are const-friendly and obey the
no-allocation frame-path rule.

### 3.7 `ui::text::EditController` (rank 7)

202-line header, 38 methods, 13 members, 383-line TU. It owns buffer edits, platform key chords, IME
composition state, the clipboard seam, undo/redo history, caret blink policy and preedit export.
**Proposal.** `text::KeyMap` (chord -> command table replacing the `if` chain in `handle_key`),
`text::CompositionState` (`preedit_`, `composition_start_`, `composition_end_`, `composing_`,
`preedit_range`) and the existing `EditHistory`, which the controller only forwards. Smallest of the
class splits and the easiest to land alone.

### 3.8 `jsx::Parser` (rank 8)

[parser_internal.hpp](../../src/compiler/analyzer/src/parser_internal.hpp) declares 78 methods and
nests `Checkpoint`, `DepthScope` and `ArrowSignature`. The definitions are already split across eleven
TUs by grammar (`parser_expr`, `parser_primary`, `parser_type`, `parser_module`, `parser_component`,
`parser_jsx`, `parser_jsx_attributes`, `parser_literals`, `parser_rules`, `bind_pairing`, `parser`),
so the TU shape is right and only the single declaration block is monolithic.

**Proposal.** Keep one class and move each grammar's declarations into a private per-grammar header
included only by its TU. That preserves the explicit recursive-descent call graph and changes no
behavior. A `ParserCore` plus free `detail` functions taking `ParserCore&` is the alternative if
per-grammar testing is ever needed.

### 3.9 `runtime::FrameDiagnostics` (rank 9)

Seventeen counters and 20 accessors over three subjects: frame counters (`frames_`, `total_steps_`,
`total_completions_`, damage/scissors/animation samples, glass cost), adaptive-quality state (five
change counters plus the sample) and the frame-time source (`frame_time_source_`, gpu/cpu sample
counts). **Proposal.** `FrameCounters`, `AdaptiveQualityCounters` and `FrameTimeSourceCounter`, with
`FrameDiagnostics` aggregating them for the existing readers (`AppHost::frame_diagnostics()` returns
the aggregate; there is no `stats()` accessor on the class). Behavior-preserving, and it
makes the three sections of [performance.md](../design/performance.md) match the code. This is the
*frame* subset and stays in `runtime`; the general logger and metric registry already live in
`tessera/diagnostics`.

### 3.10 `ui::scene::InputRouter` (rank 10)

26 methods, 11 fields, `input_dispatch.cpp` at 516 lines with a 221-line dispatch method. The
per-family files already exist (`click_synthesis.cpp`, `hover_dispatch.cpp`, `wheel_dispatch.cpp`,
`text_input_dispatch.cpp`); give each a small type and let `InputRouter` own only capture state and
the family order.

## 4. God enums and payload structs

An enum is a god enum when it is not a closed vocabulary but a **dispatch key with parallel
switches**: each new value forces edits in N places the compiler cannot relate to each other.
Counting `case` labels per enum type across the tree:

| Enum | Values | `case` sites | Files | Verdict |
|---|---|---|---|---|
| `a11y::Role` | 23 | 137 | 4 | **split: one table, four views** |
| `ir::NodeKind` | 15 | 25 | 3 | **split: kind trait table + tagged node** |
| `UiaControlType` | 20 | 20 | 1 | data mirror, fold into the same table |
| `InputEventType` | 14 | 23 | 5 | **split with `Event`** |
| `ValidationErrorCode` | 24 | 24 | 1 | keep; single formatter |
| `JsonErrorCode` x3 | 12 + 9 + 12 | 21 | 2 | **one concept, three enums: unify** |
| `EventType` | 20 | 12 | 2 | **split with `Event`** |
| `TokenType` | 31 | 0 | 1 | keep; CSS tokenizer vocabulary |
| `LogCategory` | 14 | 14 | 1 | keep; one formatter |
| `AtspiRole` / `AtspiState` | 30 / 21 | 0 | 1 | protocol mirror, keep as data |
| `Dirty`, `PseudoState` | 8 | 0 | — | keep; bit sets |

### 4.1 `a11y::Role` — four parallel switches over one enum

`Role` has 23 values and drives four independent mappings, each written as a full `switch`:
`a11y_roles.cpp` (23 `case` labels plus a reverse `role_from_string` chain),
[atspi_mapping.cpp](../../src/ui/accessibility/atspi_mapping.cpp) (76 labels across `atspi_role`,
`atspi_role_name`, `atspi_states`, `atspi_interfaces`, `atspi_layer`, `atspi_action_name`),
[uia_mapping.cpp](../../src/ui/accessibility/uia_mapping.cpp) (29 labels) and
[a11y_tree.cpp](../../src/ui/accessibility/a11y_tree.cpp) (9 labels in `actions_for`). Adding one ARIA
role means editing four files and four switches, with no compiler help if one is missed (each has a
`default` returning a generic value).

**Proposal: one `constexpr` trait table** next to `Role`, with `kCount` as the last enumerator:

```cpp
struct RoleTraits {
    std::string_view name;   // ARIA spelling
    AtspiRole atspi;
    std::string_view atspi_name;
    UiaControlType uia;
    AtspiLayer layer;
    std::uint32_t states;    // AtspiState bits derived from the role
    bool interactive;        // derives Action::Press, UIA Invoke, focusability input
};
inline constexpr std::array<RoleTraits, kRoleCount> kRoleTable = {{ /* ... */ }};
static_assert(std::size(kRoleTable) == static_cast<std::size_t>(Role::kCount));
```

Every mapper becomes an index into the table. The reverse lookup is a linear scan over 23 entries;
it runs at stylesheet/schema load, not per frame, so the cost is irrelevant and the duplicated
string list disappears. `AtspiRole` and `UiaControlType` stay as they are: they mirror protocol
numbers that must not change, and a cast-to-protocol table is the right shape for them. The defect
is the four switches, not the mirror enums.

### 4.2 `scene::Event` — 33 fields and a 256-byte inline buffer

[event.hpp](../../include/tessera/ui/scene/event.hpp) is one struct covering pointer, wheel, key,
text/IME, scroll and click payloads: 33 data members, including eight scroll metrics, a
`std::array<char, kMaxEditTextBytes>` (256 bytes) and a parallel `text_view` pointer to the same
bytes. It is declared or constructed at 21 sites in `src/`, all inside the eight scene/text TUs that
produce or forward an event. Two clean models:

- **Tagged variant.** `Event{ EventType type; Common common; std::variant<PointerPayload,
  WheelPayload, KeyPayload, TextPayload, ScrollPayload, ClickPayload> data; }`. A handler cannot read
  a wheel delta off a key event after the tag check.
- **SoA payload + typed view.** Keep `Event` as the dispatcher's unit, move the family payload into
  a separate block, and hand handlers a `PointerView`/`WheelView` that asserts the tag.

Do the buffer first: `text_view` already carries the committed full text, and only IME events need
the bounded copy. A `std::string_view` into a per-dispatch scratch buffer shrinks every `Event` by
256 bytes, which matters because events are stack-built in the input path and copied through
capture/bubble. `input::InputEvent` repeats the shape one layer down by design: it is the platform
boundary type. The rule to state explicitly is "flat at the platform boundary, tagged inside the
scene"; today both layers are flat.

### 4.3 `ir::NodeKind` with `ir::Node`

`NodeKind` has 15 kinds, while `ir::Node` carries the union of every kind's fields (`element`,
`component`, `key`, `fallback`, `item_template`, `text`, `loop`, `condition`, three maps,
`ui_id`/`automation_id`). Validation and lowering then re-check "the fields this kind allows" in
`validate_node.cpp`, `node_lowering_internal.hpp` and the emitters — 25 `case NodeKind` sites across
three files. **Proposal.** A `NodeKindTraits` table (allowed fields, takes children, is a collection,
emitter entry point) generated from the IR spec table, plus a `std::variant` of per-kind payloads for
the four dominant kinds (`Text`, `Image`/`Path`, `Component`, `Slot`); `id`, `source`, `props`,
`style` and `events` stay common. Start with the trait table: it removes the duplicated per-kind
checks without touching the versioned IR wire format.

### 4.4 Three JSON error enums

`JsonErrorCode` exists three times with overlapping values: `src/schema/src/json.hpp` (12),
`src/runtime/config_json.hpp` (9) and `include/tessera/harness/json_parse.hpp` (12, named
`JsonParseErrorCode`), each with its own formatter. The parsers are separate on purpose (project
schema, runtime config, harness protocol) but the error vocabulary is one concept: syntax, depth,
escape, UTF-8, trailing data. **Proposal.** One `json::ErrorCode` in a shared *detail* header (not a
public API header; the harness keeps its own wrapper struct), with one `to_string`. Mechanical, and
it removes a class of "the same failure has three names" bugs.

## 5. God functions

The god classes come with god functions; the longest bodies measured per file:

| Function | Lines | File |
|---|---|---|
| `emit_node` | 469 | [emit_node.cpp](../../src/compiler/codegen/src/emit_node.cpp) |
| `emit_component_header` | 311 | [emit_header.cpp](../../src/compiler/codegen/src/emit_header.cpp) |
| `emit_generated_base` | 304 | [emit_base.cpp](../../src/compiler/codegen/src/emit_base.cpp) |
| `parse_script_step` | 267 | [script_step.cpp](../../src/harness/script_step.cpp) |
| `run_cli_impl` | 264 | [cli.cpp](../../src/harness/cli.cpp) |
| `InputRouter::dispatch` | 221 | [input_dispatch.cpp](../../src/ui/scene/input_dispatch.cpp) |
| `emit_source` | 209 | [emit_component.cpp](../../src/compiler/codegen/src/emit_component.cpp) |
| `GridEngine::size_axis` | 208 | [grid_tracks.cpp](../../src/ui/layout/grid_tracks.cpp) |
| `Tokenizer::next` | 194 | [tokenizer.cpp](../../src/ui/style/tokenizer.cpp) |

`emit_node` is the highest-value split: its body is a linear sequence of emission groups separated
by comments (element kind, metadata, id, className, automationId, role, aria, style, events,
controlled wiring, refs, focusability, scroll flags, paint text, children). Each group is a natural
function, which also turns the per-node emission order from a comment block into a documented list
of calls. `AtspiBridge::handle_accessible` is the other dispatch chain worth naming: 95 lines, one
`if (member == ...)` followed by 15 `else if` branches over 16 interface members. The standard fix is
a `constexpr` table of `{name, signature, handler}` rows, the shape `atspi_properties.cpp` already
uses for its property table.

## 6. Suggested order

Each item is an independent, testable slice; none requires the others.

| Order | Slice | Risk | Why |
|---|---|---|---|
| 1 | `a11y::Role` trait table (4.1) | low | pure refactor, 137 dispatch sites collapse, mapping tests exist |
| 2 | `emit_node` emission groups (5) | low | codegen goldens prove byte-identical output |
| 3 | `FrameDiagnostics` split (3.9) | low | accessors only, no frame-path change |
| 4 | `Node` TU split, then views (3.1) | medium | biggest payoff; `node_size_tests` on every slice |
| 5 | `Event` buffer removal, then payload views (4.2) | medium | input hot path; do the buffer first and measure |
| 6 | `EditController` keymap + composition (3.7) | low | self-contained, one subsystem |
| 7 | `VirtualRange` collaborators (3.3) | medium | add direct accessors first or the split breaks callers |
| 8 | `Batched2DRenderer` state cluster (3.5) | medium | needs null-backend and golden runs |
| 9 | `WindowHost::Impl` collaborators (3.2) | medium | probe and scaled targets before presentation |
| 10 | `AppHost` stages (3.4) | low-medium | the stages are already file-shaped |
| 11 | `ir::NodeKind` traits (4.3) | high | IR is versioned; traits table only, no wire change |
| 12 | JSON error unification (4.4) | low | mechanical, but three parsers move together |

Rules for every slice: files stay under the 300/500 limits, one primary type per header,
`node_size_tests` and the codegen goldens stay green, no new allocation or vtable on a frame path,
and [docs/README.md](../README.md) plus [module-map.md](module-map.md) update with each landing.

## 7. What is deliberately not split

- `TokenType` (31 values, zero switches) is the CSS Syntax vocabulary; splitting hides the mirror.
- `AtspiRole`, `AtspiState`, `AtspiLayer`, `WireType`, `HeaderField` mirror external protocol
  numbers. They are data, not dispatch, and must keep their exact values.
- `ui::InputEvent` keeps its flat layout: a tagged variant there would push the platform's union
  into the scene.
- `Node`'s lazy payload pointers stay. The 320-byte budget and the allocation-count test encode a
  measured decision for 10k-node screens; `SceneExtras` is a later experiment, not part of the split.

## 8. Open questions

1. Is `NodeMetadataView` worth its indirection when `node_metadata.cpp` already isolates the
   implementation? It pays off only if the view replaces the forwarders in paint/scroll hot paths;
   otherwise the TU split alone is the win.
2. Does `Event` move to a variant before or after the paint-text payload work that copies into
   `Event::text`? The buffer removal is independent and cheaper.
3. `WindowHost::Impl` and `AppHost::Impl` are private headers, so their splits are invisible to
   users and can land in any order; the only constraint is that `WindowOptions` and
   `WindowFrameStats` stay put.
