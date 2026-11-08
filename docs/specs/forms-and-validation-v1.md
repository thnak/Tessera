# Forms and validation v1

Status: proposed (Big Steps C and F, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step), section "Big
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

## 9. Deferred/open questions

Each item is unresolved (no decision yet) unless it says otherwise. V1 is the first-release
scope ([AGENTS.md](../../AGENTS.md#non-negotiables) rule 8, "V1 scope discipline").

- Multi-step/wizard forms and file fields: no decision yet.
- Server-provided validation schemas/dynamic rules (not in V1).
- Draft persistence conflict merge across windows: no decision yet.
- Whether `Field` ships as a library component or an intrinsic wrapper (currently library).
- Field-level vs form-level async validation scheduling: no decision yet.
- Dynamic field arrays and keyed field identity are defined; large-array performance is open.
