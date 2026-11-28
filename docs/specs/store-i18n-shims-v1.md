# Store and i18n shims v1 (`zustand`, `react-i18next`, `i18next`)

Status: draft; coverage tables, typings and diagnostics are implemented, the lowering of the pending
names is #1045 (store) and #1046 (i18n). Issue #550, ADR-058 section 5 step 3. Rationale:
[`design/store-i18n-shims.md`](../design/store-i18n-shims.md). Package layout, the coverage-table format
and the `TESSERA_SHIM_*` diagnostics are those of [`router-shim-v1.md`](router-shim-v1.md) sections 1 and 2.

## 1. Packages

```json
{ "packages": [
  { "name": "zustand", "root": "packages/zustand", "version": "0.1.0" },
  { "name": "react-i18next", "root": "packages/react-i18next", "version": "0.1.0" },
  { "name": "i18next", "root": "packages/i18next", "version": "0.1.0" }
] }
```

Each root holds a `tessera-package.json` and an `index.d.ts` that declares only table names, each with an
`@status`. `i18next` is imported by name (`import { t } from "i18next"`): the default import is outside
the TSX subset (`TESSERA_JSX_SUBSET_IMPORT`).

## 2. Status of this version

Every covered name is **pending**: the import is recognised, and using it is
`TESSERA_SHIM_PENDING` with the form to write meanwhile. Nothing is emulated.

## 3. `zustand` coverage table

| Name | Status | Form | Lowering |
|---|---|---|---|
| `create` | pending (#1045) | `create<T>()((set, get) => ({ ... }))`, curried, `T` a record type; state fields and action fields | a generated store service over `Observable<T>` |
| the hook `useX(selector)` | pending (#1045) | selector is `(s) => s.a.b`, a field path | render-time read of one field, dependency known at compile time |
| `set(partial)`, `set((s) => partial)` | pending (#1045) | inside an action | record update on the observable |
| `get()` | pending (#1045) | inside an action | read of the observable |
| `createStore`, `useStore` | unsupported | use `create` | none |
| `shallow`, `useShallow` | unsupported | selectors compare by value | none |
| `persist`, `devtools`, `immer`, `subscribeWithSelector` (any other export) | unsupported | Storage service; devtools protocol; none | none |

## 4. `react-i18next` and `i18next` coverage table

| Name | Status | Form | Lowering |
|---|---|---|---|
| `useTranslation` | pending (#1046) | `const { t } = useTranslation();`, no namespace argument | binds `t` to the locale service |
| `t(key, values)` | pending (#1046) | `key` a string literal found in the catalog; `values` an object of scalars; `count` selects the plural | catalog read depending on the locale epoch; `{{name}}` is ICU `{name}` |
| `changeLanguage(tag)` | pending (#1046) | a string; handler or effect only | `LocaleService::set_requests` as a service call |
| `Trans`, `withTranslation` | unsupported | `t()` with values | none |
| `I18nextProvider`, `initReactI18next` | unsupported | the app root configures the service | none |
| `init`, `use`, `createInstance` | unsupported | catalog is a build input (i18n-v1); plugins do not run (ADR-024) | none |

A non-literal key, a missing catalog key and a namespace-prefixed key are `TESSERA_SHIM_FORM` once the
lowering lands.

## 5. Package manifest examples

`packages/zustand/tessera-package.json` is `{ "name": "zustand", "version": "0.1.0" }`; the version rules
and the `^` caret form are those of [`package-manifest-v1.md`](package-manifest-v1.md).

## 6. Tests

`tests/unit/shim_router_tests.cpp` (`test_the_coverage_table_diagnostics`) has one case per status for
each package: `create` and `t` and `useTranslation` are pending, `persist`, `Trans` and `init` are
unsupported, with the hint naming the replacement. The port-check class table lists the three
specifiers as class B (`tools/port-check/port_check_tests.cpp`). Tests per covered API arrive with the
lowering (#1045, #1046).
