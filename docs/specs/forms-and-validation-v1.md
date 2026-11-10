# Forms and validation v1

Status: core model implemented (task-78: bounded field registry, synchronous rule matrix, async
validator seam, submit gating) and the compiler path implemented (task-90: `Field` resolves as a
library component and its props are emitted through the ordinary component path; widgets/a11y
wiring still proposed). Big Steps C and F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
Steps"). Decision:
[ADR-029](../../decisions/ADR-029-interaction-completeness.md) (interaction completeness: text
editing, scrolling, overlays, forms). Companion to
[`text-editing-v1.md`](text-editing-v1.md), [`i18n-v1.md`](i18n-v1.md),
[`accessibility-v1.md`](accessibility-v1.md), and [`async-data-v1.md`](async-data-v1.md).

Forms are state + validation + focus + accessibility, not a collection of ad-hoc widgets.

## 1. Model

```cpp
struct FieldState<T> {
    T value;
    T initial;
    bool dirty;
    bool touched;
    bool disabled;
    bool read_only;
    std::vector<Issue> issues;   // error/warning
    bool validating;
};

struct FormState {
    bool submitting;
    bool submitted;
    bool valid;
    std::uint64_t submit_epoch;
};
```

- A form owns field state by stable field id; order is declaration order.
- Values are typed/serializable; no arbitrary objects/functions in state.
- `dirty` = value differs from initial; `touched` = user interacted/blurred.
- An epoch is a counter that identifies the current value or submit attempt; a validation or
  submit result carrying an older epoch is stale and is discarded (see section 2 and
  section 8.1).
- `valid` = no blocking errors after validation settles.
- Server/async errors map to fields or form-level issues.

## 2. Validation

- Synchronous validators: `required`, length, range, pattern, custom pure function.
- Asynchronous validators: cancellable, debounced, deduplicated by field + value epoch.
- Cross-field validators receive immutable values; no mutation during validation.
- Validation order is deterministic; stale async results are discarded by epoch.
- Errors are values with stable code/message key, severity, and optional field path.
- Validation does not throw; user-facing messages are localized (see [`i18n-v1.md`](i18n-v1.md)).
- Validate on change/blur/submit is explicit; default: blur + submit, not every keystroke for
  async.

## 3. Submit and reset

- Submit validates all fields, focuses the first invalid field, and announces an error summary.
- Submit is disabled while submitting; double-submit is deduped by epoch.
- Cancel/abort cancels in-flight validation/submission; no callback after teardown.
- Reset restores initial values/selection/errors and clears touched/dirty (opt-in).
- External value changes can reset or merge; policy is explicit per field.
- Autosave drafts are opt-in; see [`state-restoration-v1.md`](state-restoration-v1.md).

## 4. Field integration

- `TextInput`/`TextArea`, checkbox/radio/switch, select, slider, custom elements.
- Field wrapper provides label, help, error, required marker, and accessibility semantics (ARIA-style label/description/error
  relations; see [`accessibility-v1.md`](accessibility-v1.md)).
- Label/description/error ids are linked for accessibility; errors are announced.
- `autofill` hints are per field; passwords never autofill into non-secure fields.
- Disabled/read-only fields are not submitted unless explicitly included.
- Conditional fields are declarative; hidden fields keep or clear state (the choice is not specified here).

## 5. JSX shape

```tsx
<Form
  initial={{ email: "", age: 0 }}
  validate={validate}
  onSubmit={submit}
>
  <Field name="email" label="Email" required>
    <TextInput inputMode="email" autofill="email" />
  </Field>
  <Field name="age" label="Age">
    <TextInput inputMode="numeric" />
  </Field>
</Form>
```

- `Form`/`Field` are library components, not new intrinsic nodes (built-in JSX element types
  known to the compiler; see [`component-model-v1.md`](component-model-v1.md)); `TextInput`/
  `TextArea` are intrinsics.
- `validate` is a static binding/pure handler, not arbitrary JS at runtime.
- Values/errors are inspectable for tests and state restoration.
- No context/suspense dependency; field state is explicit in generated C++.

### 5.1 Field prop records (implemented)

`Field` is declared once as ordinary IR data (`src/compiler/lowering/src/library_components.cpp`)
and imported from `"tessera"`. Lowering emits a `Component` node that names `comp::Field`; there is
no intrinsic element and no Field-specific emitter, so the usage takes the same codegen path as any
project component (`docs/design/compiler.md` section 11). The documented prop records are:

| Prop | IR type | Default | Notes |
|---|---|---|---|
| `name` | `string` | `""` | Stable field path inside the form |
| `label` | `string` | `""` | Programmatic label (the library renders it) |
| `value` | `string` | `""` | Controlled value, usually an expression over form state |
| `defaultValue` | `string` | `""` | Uncontrolled initial value |
| `required` | `bool` | `false` | Required marker and submit gate input |
| `validators` | `list<string>` | `[]` | Rule ids/parameters as data; the library evaluates them |
| `onValidate` | `TextChangeEvent` (record) | none | Callback prop: bound through the node's events, not a value |
| `error` | `string` | `""` | Visible error message |
| `errors` | `list<string>` | `[]` | Visible error list |
| `children` | `children` | none | The static Children slot for the wrapped widget |

- A value prop assigns the instance's generated prop member before the instance's `build()` runs;
  `apply_props()` re-assigns them from the caller's state on every update, then the caller's
  `update()` drives the instance's `update()`, which repaints its interpolated label/value/error.
- A record-typed prop is a callback prop. `onValidate={handler}` is lowered into the node's
  `events` map and codegen binds `std::function<void(const TextChangeEvent&)>`. The handler must
  take exactly the declared payload record (one parameter), so the callback carries the field value.
- Unknown prop names, a value passed for a callback prop (or a callback bound for a value prop), a
  literal of the wrong type, and a handler with the wrong payload are codegen errors carrying the
  prop name and the component, exactly like any other component prop.
- Validation is not generated: `required`/`validators` reach `Field` only as prop data, and the
  `FormController`/rule engine (`src/ui/forms/`) evaluates them. `forms_codegen_tests` asserts the
  generated library translation unit contains no rule evaluation and the caller carries the rule
  string only as a prop record.
- Children ride the static `Children` slot (`ir-v1.md` section 3.4): one widget position, built by
  the caller and attached by `Field` at its Slot.
- Tests: `forms_lowering` (import resolution, `Component` IR node kind, prop-record mapping,
  slot, determinism), `forms_codegen` (prop/callback matrix, unknown-prop and type diagnostics,
  golden, determinism, merger idempotency), `forms_codegen_runtime` (two Fields in a form, a value
  change through the generated paths, no GPU).

## 6. Accessibility

- Every field has a programmatic label; placeholders are not labels.
- Errors are associated with the field and announced once; error summary links to fields.
- Focus first invalid; focus order follows visual/logical order.
- Required/disabled/read-only/invalid states are exposed.
- Touch targets and text scale follow accessibility rules.
- Async validation announces busy/done at a bounded rate.

## 7. Testing

- Field state transitions (dirty/touched/valid/submitting).
- Sync/async validation, stale results, cancellation, debounce.
- Cross-field rules and dependency order.
- Submit success/failure, focus first invalid, double-submit, reset.
- Autofill/password/secure behavior; disabled/conditional fields.
- Accessibility snapshots and error announcements.
- Headless form scripts; deterministic async with fake clock.

## 8. Security and privacy

- Never log passwords/secure values; redact snapshots/traces.
- Validate all input server-side too; client validation is UX.
- Bound field sizes, error lists, and async validators.
- Treat pasted/dropped content as untrusted.
- No arbitrary validators from runtime data; validators are compiled handlers.

## 8.1 Detail design

Ownership:

| Component | Owns | Does not own |
|---|---|---|
| `FormController` | field registry/order, values, submit state, focus-first-invalid | Widget rendering |
| `FieldState<T>` | value/initial/dirty/touched/issues/epoch | Async transport |
| `ValidationPipeline` | sync/async validation scheduling, cancellation | Error localization |
| `TextInput/TextArea` | editing state/selection/IME (input method editor) | Form value ownership |
| `Field` library component | label/help/error/aria wiring | Validation rules |

Recommended field types: `bool`, `int`, `float`, `string`, `enum`, `date/time`, `uuid`,
`file-ref`, and arrays of these. Values are serializable; no closures/pointers. Field identity
is a stable path (`billing.address.line1`, `items[0].sku`) with keyed arrays.

State machine per field:

```text
Pristine -> Dirty -> Validating -> Valid
                         \-> Invalid
Dirty/Valid/Invalid -> Submitting -> Submitted
                         \-> SubmitError
Reset -> Pristine (initial values, selection, issues)
```

Validation pipeline:

```text
value/props change -> sync validators -> visible issues
  -> schedule async validator (debounce/cancel by epoch)
  -> apply only if (field, value, form epoch) still current
  -> cross-field validators run after field results settle
```

- Sync validators are pure and fast; async validators are cancellable and deduped.
- Cross-field validation receives an immutable value snapshot; cycles are rejected at build
  time or evaluated in a fixed topological order.
- Server errors map by field path + code; unknown paths become form-level issues.
- Validation does not submit; submit runs validation and waits for settling.

JSX/library contract:

```tsx
<Form initial={initial} validate={validate} onSubmit={submit}>
  <Field name="email" label="Email" required>
    <TextInput inputMode="email" autofill="email" />
  </Field>
</Form>
```

- `Form` owns values; `Field` reads/writes by name; `TextInput` remains controlled internally
  through the field binding.
- Arrays use stable keys; adding/removing/reordering preserves field state by key.
- `reset` restores initial values/selection/issues; `resetField` is explicit.
- `submit` is deduped by epoch; abort cancels validation/submit and shows no stale error.

Failure/races:

- Async validation result after value change/unmount is discarded by epoch.
- Reset during submit aborts submit and invalidates its epoch.
- Double-submit returns the same in-flight result or is ignored (which one is not specified here).
- External value updates merge only for untouched fields by default; touched fields keep user
  input and surface a conflict.
- Autofill is a normal value change with a distinct source for UX/diagnostics.

Gap-closure decisions:

Gap IDs below (`G-10`, `G-11`) refer to the entries in
[`app-completeness-gap-resolutions-v1.md`](../architecture/app-completeness-gap-resolutions-v1.md)
(resolutions of gaps found by the app-completeness cross-check).

- **G-10**: form schema/types are generated from field declarations; nested paths and keyed
  arrays are supported; cross-field validators run in topological order and cycles are compile
  errors.
- **G-11**: server errors map to `Issue{path,code,severity,message_key}`; reset/submit use
  epochs; autofill is a value source. Untouched fields may merge external values; touched fields
  keep user input and surface a conflict issue.

## 8.1 Implemented core (task-78)

`include/tessera/ui/forms/form_types.hpp` + [`form_model.hpp`](../../include/tessera/ui/forms/form_model.hpp)
(library `tessera_forms`) implement the bounded deterministic core; widgets, generated bindings and
accessibility wiring are not part of this slice.

- **Model:** `FieldId` is stable and assigned 1..N in declaration order; `FormValue` is string
  (bounded by `FormConfig::max_value_bytes`, default 4096), enum (`int32`) or bool; each field has
  value/initial, dirty/touched/disabled/read-only, `ValidationState` (`Unknown/Valid/Invalid/
  Pending`) and a bounded issue list. Caps: `max_fields = 32`, `max_rules_per_field = 8`,
  `max_issues_per_field = 4` (stored, in rule order) with `issue_count` retaining the total,
  `max_name_bytes = 64` (names truncate at a UTF-8 boundary), `max_value_bytes = 4096` (larger
  values are rejected), plus code/message/pattern caps. Adding beyond a cap returns false/nullopt.
- **Rules:** `required` (string non-empty / bool true / enum >= 0), min/max length counted in UTF-8
  code points (strings only; other kinds get `length_expects_string`), `pattern` glob (`*` any run,
  `?` one code point, literals otherwise; strings only), integer `numeric_range` (strictly parsed
  string or enum; `not_numeric` otherwise), `equal_to` another field's current value (dependent
  fields are re-evaluated once, non-transitively, so cycles cannot recurse), and a pure `custom`
  callback returning an issue value or nullopt (a throw maps to `custom_exception`). Synchronous
  evaluation runs on value change, touch/blur, explicit validation and submit; results are values
  and the same input gives the same output.
- **Async seam:** `AsyncValidateFn = std::function<runtime::Task<bool>(TaskContext, const
  FormValue&)>` runs through a bridge coroutine on `FormConfig::completion_queue`; the form never
  blocks. Pending is visible, completion is driven by the existing queue drain (single-thread UI
  affinity: drain on the UI thread), stale results are discarded by the per-field value epoch, a
  value change cancels the in-flight token, and the destructor cancels everything so no callback
  runs after teardown. Default trigger is blur + submit; `validate_async_on_change` opts into
  per-change scheduling (debouncing is deferred). Without a queue, async rules are ignored and the
  synchronous result stands.
- **Submit:** `can_submit()` is true only when every enabled field is `Valid` and touched
  (`require_touched_for_submit`); `submit()` validates, returns `Pending` with the pending count
  while async work is in flight, `Invalid` with the first blocking/untouched field, or `Ok`. The
  model performs no I/O (the caller performs the request) and never partially submits. `reset()` /
  `reset_field()` restore initial values, clear dirty/touched/issues and cancel async work; disabled
  fields are excluded from gating.
- **Tests:** `forms_model` (rule matrix and boundaries, policy/flags, error caps/order, field/
  rule/config caps, submit paths, determinism) and `forms_async` (pending visibility, queue-driven
  completion, async invalid, stale discard, cancellation, on-change policy, submit while pending,
  no-queue fallback, destructor safety, determinism).
- **Compiler path (task-90):** `Field` is IR data (`make_library_component`) injected when a file
  imports it from `"tessera"`; a project component of the same name wins. Lowering emits an
  ordinary `Component` node with the prop records of section 5.1, codegen assigns the value props
  and binds the `onValidate` callback prop (section 5.1), and the caller's widget rides the static
  `Children` slot. Tests: `forms_lowering`, `forms_codegen`, `forms_codegen_runtime`; goldens
  `tests/golden/codegen/{Field,FormPage}.*`.
- **Deferrals:** cross-field dependencies beyond `equal_to`, localized messages, accessibility
  error wiring/announcements, server-side validation, form-level async rules, debounce/dedup,
  conditional fields, field arrays/keyed identity and widgets/generated bindings.

## 9. Deferred/open questions

Each item is unresolved (no decision yet) unless it says otherwise. V1 is the first-release
scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8, "V1 scope discipline").

- Multi-step/wizard forms and file fields: no decision yet.
- Server-provided validation schemas/dynamic rules (not in V1).
- Draft persistence conflict merge across windows: no decision yet.
- Whether `Field` ships as a library component or an intrinsic wrapper: **resolved** - library
  component, no intrinsic node kind; the compiler emits its props through the ordinary component
  path (section 5.1, Q-09).
- Field-level vs form-level async validation scheduling: no decision yet.
- Dynamic field arrays and keyed field identity are defined; large-array performance is open.
