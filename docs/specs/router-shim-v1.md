# Router shim v1 (`react-router-dom` subset)

Status: draft, first slice implemented (issue #549, ADR-058 section 5 step 3). Rationale:
[`design/router-shim.md`](../design/router-shim.md). Terms: **shim** and **coverage table** as in the
design note; **pending** is a covered name whose lowering has not landed.

## 1. Package

A shim is an ordinary package ([`package-manifest-v1.md`](package-manifest-v1.md)): a directory with a
`tessera-package.json` and typings, mapped in `tessera.json`.

```json
{ "packages": [{ "name": "react-router-dom", "root": "packages/react-router-dom", "version": "^0.1.0" }] }
```

`packages/react-router-dom/index.d.ts` is the published typing: only names of the coverage table are
declared, each with an `@status`. The specifiers `react-router-dom` and `react-router` name the same
table. The front end owns the specifier whether or not the project maps it, so the diagnostics below
do not depend on the mapping; the mapping gives the editor the typings.

## 2. Coverage-table format (shared by every shim)

One row per exported name: **Name**, **Status** (`supported`, `pending`, `unsupported`), **Form** (the
accepted spelling), **Lowering** (what it becomes). A name absent from the table is
`TESSERA_SHIM_UNSUPPORTED`. The table's data lives in `src/compiler/analyzer/src/shim_table.cpp`; this
spec and that file are the same facts and the shim tests name every row.

| Code | When | The message always carries |
|---|---|---|
| `TESSERA_SHIM_UNSUPPORTED` | an imported name that is outside the table or has status `unsupported` | the replacement or the section to read |
| `TESSERA_SHIM_FORM` | a supported name written in a form outside the table | the accepted form |
| `TESSERA_SHIM_PENDING` | an imported name with status `pending` | the form to write until it lands, and the issue |

## 3. Coverage table

| Name | Status | Form | Lowering |
|---|---|---|---|
| `useNavigate` | supported | `const navigate = useNavigate();` at the top of a component | `useService<RouterService>()` |
| `navigate(to)` | supported | one expression argument, a path string | `navigate.push(to)` |
| `navigate(to, { replace })` | supported | `replace` as a boolean literal, no other option | `.replace(to)` or `.push(to)` |
| `navigate(-1)` | supported | exactly `-1`, no options | `navigate.back()` |
| `Routes`, `Route`, `Outlet` | pending (#1044) | declare the table in C++ (`RouteTable`) | route table and a route-switched child |
| `useParams`, `useLocation` | pending (#1044) | read from the entry once the observable read lands | render-time read of `NavController::route()` |
| `Link`, `NavLink` | pending (#1044, #542) | `View onClick={() => navigate("/p")}` meanwhile | internal `href`, shared with `a href` |
| `BrowserRouter`, `HashRouter`, `MemoryRouter`, `RouterProvider`, `createBrowserRouter` | unsupported | delete; the app root provides the controller | none |
| `useSearchParams` | unsupported | a typed route param (`navigation-v1.md` section 1) | none |
| `Navigate` | unsupported | `navigate(path, { replace: true })` from an effect or handler | none |
| `useLoaderData`, `useNavigation` | unsupported | `useResource`, `useTransition` (`pending-ui-v1.md`) | none |
| `useBlocker` | unsupported | a guard on the controller (`navigation-v1.md` section 4) | none |
| `Form`, `ScrollRestoration` | unsupported | the library `Form`; the restore snapshot | none |

`navigate(n)` with another number, a location object (`{ pathname }`), a state/relative/viewTransition
option, no argument and more than two arguments are `TESSERA_SHIM_FORM`. `useNavigate` with an
argument, as a destructuring or inside a custom hook is `TESSERA_SHIM_FORM`.

## 4. Lowering

```tsx
const navigate = useNavigate();
<Button onClick={() => navigate(`/orders/${id}`)} />
```

becomes, before the rules run, `const navigate = useService<RouterService>();` and
`navigate.push(...)`. The shim import is removed. From there the service rules of
[`services-manifest-v1.md`](services-manifest-v1.md) apply unchanged: the call is accepted in a handler
or an effect and is `TESSERA_REACT_SERVICE_CALL_IN_RENDER` while rendering. An effect's dependency list
loses `navigate`. Lowering adds the usual `ir::Component::services` entry (id `tessera.router`, type
`tessera::appfw::RouterService`, header `tessera/appfw/router_service.hpp`) and one `callService`
action; codegen emits the typed accessor and `static_assert` on the id.

`RouterService` is built in: `useService<RouterService>()` resolves without a `ui/services.json` entry,
and a manifest entry named `RouterService` takes precedence.

## 5. Runtime semantics

| Method | Controller call | Notes |
|---|---|---|
| `push(path)` | `NavController::push` | route match, typed params and the route's duplicate policy apply |
| `replace(path)` | `NavController::replace` | history does not grow |
| `back()` | `NavController::back` | modal/sheet first, then pop; at root is reported, not an error |
| `last_error()` | none | the last refusal (`NotFound`, an invalid param, `CapacityExceeded`), cleared by the next accepted request |

Requests commit at the frame boundary, like every controller request. Deep links and restore are
controller features; the service navigates on the stack they produce.

## 6. Tests

- `tests/unit/appfw_router_service_tests.cpp`: route match with params, a refused path recorded and the
  stack untouched, replace keeps depth, back, a cold deep link then back and push.
- `tests/unit/shim_router_tests.cpp`: every supported form lowers to the expected `callService` action
  and requirement (no manifest), an alias and the `react-router` specifier, the evaluator reaches the
  sink, each rejected `navigate`/`useNavigate` form, and one diagnostic per table status with its hint.
- `tests/unit/jsx_diagnostics_cases.hpp`: a fixture per `TESSERA_SHIM_*` code and the golden.

## 7. Versioning

The package version follows [`package-manifest-v1.md`](package-manifest-v1.md). Moving a name from
pending to supported is additive; removing a supported name or changing its lowering is a new spec
version.
