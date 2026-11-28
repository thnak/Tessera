# Store and i18n shims: `zustand` and `i18next` over Observable and the locale service

Status: design note for issue #550 (ADR-058 section 5 step 3). Normative text:
[`store-i18n-shims-v1.md`](../specs/store-i18n-shims-v1.md). Conventions (package layout, coverage-table
format, the three `TESSERA_SHIM_*` codes) are established by the router shim,
[`router-shim.md`](router-shim.md). Terms: **Observable** is `tessera::appfw::Observable<T>`
([`observable.md`](observable.md)); the **locale service** is `LocaleService`
([`i18n-v1.md`](../specs/i18n-v1.md)).

## Store: zustand-shaped, over Observable

A React project keeps cross-component state in a module-level `create` call:

```tsx
const useCart = create<Cart>()((set) => ({ count: 0, add: () => set((s) => ({ count: s.count + 1 })) }));
const count = useCart((s) => s.count);
```

The Tessera counterpart of a store is an `Observable<T>` owned by the application scope and read
through a service: it coalesces writes per frame, wakes subscribers once per flush and never allocates
per change ([`observable.md`](observable.md)). The shim is therefore a *typing and lowering contract*
over that, not a second state container:

- `create<T>()(initializer)` declares a store: a record type `T` (ADR-051) and an initializer whose
  non-function fields are the initial value and whose function fields are actions. It lowers to a
  generated `StoreService` interface per store with one `Observable<T>` and one method per action, so
  a test or the Studio preview substitutes a fake exactly as for any service (ADR-050).
- The hook call `useCart((s) => s.count)` is a render-time read of one field: the selector is a path
  (`s.field`, `s.a.b`), never code, so the dependency is known at compile time and compares by value.
  Reading needs the render-time observable read that the services manifest reserves
  (`observables`, ADR-050 open question 6); that is why `create` is **pending** (#1045) and is
  recognised and rejected with the form to use meanwhile.
- `set(partial)` and `set((s) => partial)` become a record update on the observable; `get()` reads it.
  Actions are handler bodies in the existing subset (setter-like statements), so no new statement kind.
- Middleware (`persist`, `devtools`, `immer`, `subscribeWithSelector`) is outside the table:
  persistence is the Storage service, devtools is the devtools protocol, immer has no mutation model
  in a statically typed record.

Alternatives rejected: a JS-shaped store object (no JS runtime, ADR-024); lowering every store to
`useState` lifted to the root (loses the cross-component write coalescing and the preview/native
parity of a service).

## i18n: `react-i18next` subset over the locale service

`useTranslation().t("greeting", { name })` is the dominant spelling. The locale service already owns
the resolved locale, a compiled message catalog with a fallback chain, an epoch that advances only on
a real change and a `changed()` observable ([`i18n-v1.md`](../specs/i18n-v1.md) sections 1, 5, 8).
The shim maps names to those and does not run i18next:

- `t(key, values)` with a **literal key**: the key is checked against the catalog at build time and
  the call lowers to a catalog read that depends on the locale epoch, so a locale change repaints
  once. A non-literal key is `TESSERA_SHIM_FORM` (a key that cannot be checked cannot be extracted).
- Interpolation `{{name}}` maps to the ICU MessageFormat argument `{name}` of i18n-v1 section 2; the
  catalog is ICU, so a ported JSON resource is converted by a one-time tool, not at run time.
- Plurals: `t("items", { count })` selects the ICU `plural` form of the key; i18next's `_one`/`_other`
  suffix keys are converted with the resource.
- `changeLanguage(tag)` is a service call (`LocaleService::set_requests`).
- Everything that configures i18next at run time (`init`, `use`, backends, detectors,
  `createInstance`, namespaces) is outside the table: the catalog is a build input and the app root
  configures the service in C++.

This also needs two things Tessera does not have: an IR expression that reads a catalog message (the
IR has no text-from-catalog kind) and the ICU MessageFormat slice of #195 (i18n-v1 section 2 is
designed, not built). So `useTranslation`, `t` and `changeLanguage` are **pending** (#1046).

## What this slice ships

The coverage tables, the typed packages (`packages/zustand`, `packages/react-i18next`,
`packages/i18next`), the import checks (`TESSERA_SHIM_PENDING` and `TESSERA_SHIM_UNSUPPORTED` with the
form to use now), the port-check rows (class B), and the tests of each rejected form. The lowering of
the covered APIs is #1045 and #1046; neither needs a new engine, but both need the observable read and,
for i18n, the catalog-read IR expression and the MessageFormat slice.

## Open

- Whether the one-time resource conversion tool (i18next JSON to ICU catalog) is part of #1046 or its
  own issue.
- Whether a store may be declared outside the application root (a module-level `create` in a
  component library); the service scope says no, the React habit says yes.
