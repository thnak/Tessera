# ADR-075: Server data in TSX: a `fetch` shim and a query layer, both over the one net client

- Status: Proposed (2026-11-26). The owner decided the two scope questions on 2026-11-26: the
  `fetch` shim is in, and the query layer is **V1**. The shapes below need the owner's answers in
  "Questions for the owner" before this is Accepted.
- Date: 2026-11-26
- Related: [ADR-058](ADR-058-react-vite-port-compatibility-tiers.md) (class B shim packages; its
  server-data row and section 5 step 5 are what this ADR schedules),
  [ADR-052](ADR-052-networking-platform-stacks.md) (platform HTTP stacks),
  [ADR-050](ADR-050/README.md) (services, `useService`),
  [ADR-067](ADR-067-pending-ui-transitions-suspense-lazy.md) (`useResource`, the suspending read of the
  same resource), [ADR-040](ADR-040-generated-base-and-developer-class.md) (`bind::` boundary),
  [ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md),
  [networking-v1](../docs/specs/networking-v1.md), [async-data-v1](../docs/specs/async-data-v1.md),
  [studio-mock-services-v1](../docs/specs/studio-mock-services-v1.md),
  [networking-module](../docs/design/networking-module.md)
- Terms: the **net client** is `NetClient` over a `NetTransport`. A **HTTP service** is the abstract
  interface an app resolves with `useService` (section 1). A **query** is a keyed, cached resource read
  (async-data-v1 section 1).

## Context

The net client exists with three back ends landing (WinHTTP, libcurl, the contract fakes) and the
async-data spec defines resources, cache keys, fetch policies and mutations, with a runtime data store
behind it. Neither has a TSX surface. ADR-058 says server data is C++ only and lists `fetch`, axios,
TanStack Query and SWR as not running (no JS runtime, ADR-024). Ported React code reaches the network in
two ways: a bare `fetch` in an effect or handler, and a query library over `fetch`. The owner wants
both ported with small edits.

Facts that bound the design: `networking-v1` section 2 is already `fetch`-shaped (a 4xx/5xx is a
response, not an error; a network failure is the error; cancellation is a token). The handler subset has
no `await` and no statements beyond `if`/`const`/`return`. There is one HTTP engine and it must stay
replaceable for tests and the Studio preview.

## Decision

### 1. One HTTP service interface; the net client is its default implementation

An abstract `HttpService` (`send(request, token)`, the networking-v1 `Request`/`Response`/`NetError`
types, nothing more) is registered in the app's service table. `NetClient` is the default
implementation. Everything below, and every developer-written C++, goes through it, so a test or the
Studio preview substitutes a fake and a developer can wrap or replace it (retry policy, auth header,
caching). Redirects, the cross-origin `Authorization` rule and the size cap stay in `NetClient`: a custom
implementation is a deliberate whole replacement, not a way to skip them by accident.

### 2. A `fetch` shim (class B, no import needed)

The global `fetch` is recognized by the analyzer, as the hook names are. Accepted form, in handlers
and effects: `fetch(url[, init]).then(r => r.json()).then(setter)` with an optional `.catch(setter)`.

- `init`: `method`, `headers` (object literal of strings), `body` (a string or a record serialized as
  JSON), `signal`. Anything else is a diagnostic naming the supported form.
- `Response` subset: `ok`, `status`, `url`, `headers.get(name)`, `json()`, `text()`. `json()` is typed
  by the setter it flows into (a `useState<Item[]>` gives a generated decoder for `Item[]`); a decode
  mismatch is a rejection with a typed error, never an untyped value.
- `AbortController`/`signal` map to the effect cleanup's cancellation token. Unmounting cancels.
- A 4xx/5xx resolves with `ok: false`, as in the browser; only `NetError` rejects.
- Lowering is an IR action that calls the `HttpService` and routes the typed result to the setter on the
  UI thread through the completion queue. No Promise object exists at runtime.
- Not accepted, each a diagnostic with the replacement: `await`/`async` (use the chain or `useQuery`),
  `FormData`, streams, `Request`/`Response` constructors, `credentials`/`mode`/`cache` options.

### 3. A query layer, `tessera/query` (class B shim package, V1)

A package with the `@tanstack/react-query` names, over the async-data-v1 resource model and the existing
data store (not a second cache):

- `useQuery({ queryKey, queryFn, staleTime, enabled })` returns `data`, `error`, `isPending`,
  `isFetching`, `isError`, `status`, `refetch`. `status` maps from `DataStatus`: `Loading` to
  `pending`, `Refreshing` to `isFetching` with data kept, `Error` to `error`.
- `useMutation({ mutationFn, onSuccess })` returns `mutate`, `isPending`, `error`; `QueryClient`
  offers `invalidateQueries(key)` and `setQueryData`. Invalidation lists are declared (async-data-v1
  section 4), not inferred.
- `queryFn` is either a `fetch` chain (section 2) or a `bind::` method on the developer class, which
  is how non-trivial logic stays typed on both sides.
- `queryKey` is a tuple of typed values; user/tenant scope is mandatory in the key as async-data-v1
  section 7 requires, and a missing scope is a diagnostic, not a default.
- Default fetch policy is stale-while-revalidate with the spec's TTL and deterministic eviction.
- **Coverage table, no silent emulation:** infinite queries, `useQueries`, `select`, optimistic updates,
  persisters and devtools are not in V1; each is a diagnostic naming the supported form.
- `useResource` (ADR-067) stays the suspending read of the same resource; `useSuspenseQuery` is deferred
  until ADR-067 is Accepted, and then is a thin alias.

### 4. Out of scope

axios, SWR, GraphQL clients and WebSocket/SSE stay class D with a named replacement (the `HttpService`
from C++, or a later shim). A project may ship its own shim package on the same mechanism.

## Consequences

- A ported app keeps its `fetch` and `useQuery` call shapes; the edits are typing (a result type on the
  state or the query) and rewriting `await` into the chain.
- Design-time preview and tests run the same code against a fake `HttpService`, so the Studio mock
  services (ADR-055) cover server data without a new mechanism.
- Cost: two promised API surfaces. The coverage table and the rule that unsupported calls are
  diagnostics bound it to what is tested.
- Typed `json()` decoding adds generated code per record type used; measure it with the memory budget.
- No JS runtime is introduced and the IR stays additive.

## Alternatives

- **Only the query layer, no `fetch` shim.** Rejected: bare `fetch` in effects is the most common
  ported pattern and would be a hard stop.
- **Run a JS `fetch` in the app.** Rejected by ADR-024.
- **Let the query layer own its own cache.** Rejected: two caches with two invalidation rules; the data
  store is already the resource model.
- **Accept `await`/`async` in handlers.** Not rejected forever, but it needs a handler-subset ADR and
  suspension semantics; the chain form needs neither.

## Questions for the owner

1. Is the chain form (`.then`/`.catch`) the right accepted `fetch` surface, with `await` left to a
   later handler-subset ADR?
2. Is typed `json()` (decoder derived from the receiving state type) acceptable, or must a type argument
   be written at the call?
3. Is the V1 query subset in section 3 the right cut, in particular mandatory scope in `queryKey`?
4. Should `HttpService` live in `appfw` next to `NetClient` (proposed) or in a services module?

## Follow-up

- When accepted: a spec `docs/specs/query-v1.md` (API and coverage table), a `fetch` section in
  `jsx-subset-v1.md`, the `HttpService` interface and tests, then the shim packages and analyzer rules,
  each its own issue under V1-M5.
- Update the server-data row of ADR-058 and `react-compat.md`.
