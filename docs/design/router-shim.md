# Router shim: `react-router-dom` over `NavController`

Status: design note for issue #549 (ADR-058 section 5 step 3). The normative text is
[`router-shim-v1.md`](../specs/router-shim-v1.md); this note is the why. Terms: a **shim** is a
class B package of [ADR-058](../../decisions/ADR-058-react-vite-port-compatibility-tiers.md): the npm
import specifier and a documented API subset are kept, the implementation is native. A **coverage
table** is the spec's list of names with a status, and the front end's only opinion about a shim.

## Problem

A React + Vite project almost always routes with `react-router-dom`. Tessera already has the engine:
`RouteTable` (patterns, typed params), `NavController` (push, replace, back, a published route
`Observable`), deep links and restore ([`navigation-v1.md`](../specs/navigation-v1.md),
[`navigation.md`](services/navigation.md)). What a port lacks is a TSX surface, so every ported
component that calls `useNavigate` stops at an unresolved import.

## What a port keeps, what it edits

React Router is a component tree that *declares* routes (`<Routes><Route .../></Routes>`) and hooks
that *read* the location. Tessera keeps routes as data in the app root (`RouteTable`, one controller
per window) and components talk to the controller through a service. The shim bridges the two without
emulating a browser history:

| React Router idea | Tessera meaning |
|---|---|
| `navigate(to, { replace })` | `RouterService.push` / `replace`: the controller's request, committed at the frame boundary |
| `navigate(-1)` | `RouterService.back`: the controller's back, in its section 10.1 priority |
| `<Routes>`, `<Route>`, `<Outlet>` | the route table and a route-switched child (pending, #1044) |
| `useParams`, `useLocation` | a render-time read of the route observable (pending, #1044) |
| `<Link>`, `<NavLink>` | internal `href` of #542 (ADR-059 group B), one behaviour for `a href` and `Link` (pending) |
| `BrowserRouter` and the other providers | deleted: the app root provides the controller |

## Decisions

1. **The first slice is the hook that lowers onto what exists.** `useNavigate` is
   `useService<RouterService>()` plus a handler-only service call (ADR-050 section 3.1, issue #177).
   No IR kind is added, the rules already forbid a call while rendering (as React Router does), and the
   preview evaluator already records service calls. Everything else in the table needs a render-time
   read of an observable, which the services manifest reserves (`observables`, ADR-050 open question 6)
   and does not yet implement; pending names are therefore *recognised* and rejected with the form to
   write meanwhile, never accepted and mis-lowered.
2. **A rewrite pass, not a new analysis.** `expand_shims` (`src/compiler/analyzer/src/shim_expansion.cpp`)
   runs where custom-hook expansion runs, before the rules. It checks imports against the coverage
   table, rewrites `useNavigate` to `useService` and `navigate(...)` to a method call, and removes the
   shim import (the package has no source to resolve). Rules, lowering and code generation see an
   ordinary component, which keeps preview and native in lockstep (ADR-036 item 4).
3. **The interface is not the controller.** `RouterService` (`include/tessera/appfw/router_service.hpp`)
   has `push`, `replace`, `back` and `last_error`. `NavRouterService` adapts a `NavController`; a test
   registers a recording fake and an app may register a guard. A refused request (unknown path,
   invalid param, full stack) is recorded as `last_error()`, because `navigate` in React Router is
   fire-and-forget and a handler cannot catch.
4. **The service is built in.** `RouterService` is visible to `useService<T>()` without a
   `ui/services.json` entry (`builtin_services.hpp`); a manifest entry with the same name wins, which is
   how a project substitutes its own interface.
5. **`navigate` is dropped from an effect's dependency list.** React lint rules ask for it; here it is a
   service binding, not a value, so the pass removes it and the exact-dependency check runs on what
   is left.
6. **Never silent.** Three codes: `TESSERA_SHIM_UNSUPPORTED` (outside the table, hint names the
   replacement), `TESSERA_SHIM_FORM` (a covered name in a form outside the table, such as
   `navigate(2)` or a location object), `TESSERA_SHIM_PENDING` (in the table, lowering not landed, hint
   names the form to use now).

## Alternatives rejected

- **A history API emulation** (`window.history`, hash routes): there is no DOM (ADR-058 class D) and
  the controller already has the semantics React Router approximates.
- **`navigate` as a first-class IR action**: a closed IR (ADR-036) would grow for one package; the
  service call is the existing extension point.
- **Lowering `<Routes>` first**: it is the headline feature but needs the render-time read of the route
  observable; shipping the hook first gives ports a compiling `navigate` today and keeps the table
  honest about the rest.

## Deep link and restore

They are controller features and need no shim: a deep link installs through `install_deep_link`, a
restore through `NavRestoreStore`, and `RouterService` navigates on whatever stack they produced
(`appfw_router_service_tests` covers the cold deep link then `back`).

## Open

- `Routes`/`Route`/`Outlet`, `Link`/`NavLink`, `useParams`/`useLocation`: #1044, blocked on the
  observable read and on #542.
- `navigate(n)` for `n < -1` could lower to `pop_to`; not decided, rejected with a form hint today.
