# Forms and validation v1

Status: proposed (Big Step C/F). Decision: ADR-029. Companion to
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
- `valid` = no blocking errors after validation settles.
- Server/async errors map to fields or form-level issues.

## 2. Validation

- Synchronous validators: `required`, length, range, pattern, custom pure function.
- Asynchronous validators: cancellable, debounced, deduplicated by field + value epoch.
- Cross-field validators receive immutable values; no mutation during validation.
- Validation order is deterministic; stale async results are discarded by epoch.
- Errors are values with stable code/message key, severity, and optional field path.
- Validation does not throw; user-facing messages are localized (`i18n`).
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
- Field wrapper provides label, help, error, required marker, and `aria`/semantics.
- Label/description/error ids are linked for accessibility; errors are announced.
- `autofill` hints are per field; passwords never autofill into non-secure fields.
- Disabled/read-only fields are not submitted unless explicitly included.
- Conditional fields are declarative; hidden fields keep or clear state by policy.

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

- `Form`/`Field` are library components, not new intrinsic nodes; `TextInput`/`TextArea` are
  intrinsics.
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

## 9. Open questions

- Schema/type generation from IR vs hand-written validators.
- Field-level vs form-level async validation scheduling.
- Draft persistence and multi-window conflict policy.
- Whether `Field` is a library component or an intrinsic wrapper.
- Dynamic field arrays and keyed field identity.
