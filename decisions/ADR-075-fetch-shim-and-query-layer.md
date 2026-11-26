# ADR-075: Server data in TSX: a `fetch` shim and a query layer, both over the one net client

- Status: Proposed (2026-11-26). The owner decided on 2026-11-26: the `fetch` shim is in; the query
  layer is **V1**; the `.then` chain is the V1 surface; `json()` carries its type at the call;
  `queryKey` scope is mandatory; `HttpService` lives next to `NetClient` in `appfw`. One question
  (variant types, below) is still open before this is Accepted.
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
- `Response` subset: `ok`, `status`, `url`, `headers.get(name)`, `json()`, `text()`. `json<T>()` carries its type
  at the call (`r.json<Item[]>()`); the analyzer generates a decoder for `T`. When `T` is not written it is
  inferred where unambiguous: from the setter the result flows into (`useState<Item[]>`) or from the
  `useQuery` result type; a bare `json()` with no inferable type is a diagnostic that prints the form. The
  cast `r.json() as Promise<Item[]>` (or `as Item[]` on the awaited value) is accepted at this one site as
  the type argument, since it is the common ported spelling. Existing `interface` and string-enum API
  types work as `T` through ADR-058 rule 1. Decoding is lenient like TypeScript: extra JSON fields are ignored, a missing field is `null` when its type is `T | null`, a JSON number decodes into `float` always and into `int` only when integral, and a mismatch (including an unknown string literal) rejects with a typed error, never an untyped value. `tessera port-check` prints the `json<T>()` line to write, and the flat-record rewrite for a variant response.
  `T` is any type the subset accepts today: scalars, records (ADR-051), lists, `T | null` and string-literal
  unions (an unknown literal is a decode error). **Variant types** (a discriminated union such as
  `{kind:"a",..} | {kind:"b",..}`) are not accepted by `jsx-subset-v1.md` (a union is `T | null` or string
  literals only), so they are a diagnostic. V1 workaround: a flat record with nullable fields, or a
  `bind::` method that decodes in C++. Whether to add variant types is its own decision (question 1).
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
- Typed `json<T>()` decoding adds generated code per record type used; measure it with the memory budget.
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

1. **Open.** Real APIs return variants (error vs success bodies, tagged events). Do we add discriminated
   union types to the subset and IR (a separate ADR, additive to ADR-051), or hold V1 to the workaround
   above? Until decided, `json<T>()` rejects them.

Decided on 2026-11-26: the `.then`/`.catch` chain is the V1 `fetch` surface (`await` waits for a
handler-subset ADR); `json<T>()` carries the type; the V1 query subset stands, with a mandatory scope in
`queryKey`; `HttpService` lives in `appfw` next to `NetClient`.

## Follow-up

- Check the variant-type answer against `jsx-subset-v1.md` section 5 before the decoder is built.
- When accepted: a spec `docs/specs/query-v1.md` (API and coverage table), a `fetch` section in
  `jsx-subset-v1.md`, the `HttpService` interface and tests, then the shim packages and analyzer rules,
  each its own issue under V1-M5.
- Update the server-data row of ADR-058 and `react-compat.md`.
