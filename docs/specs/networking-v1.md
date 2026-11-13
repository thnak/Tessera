# Networking v1

Status: accepted (2026-11-13, owner; [ADR-052](../../decisions/ADR-052-networking-platform-stacks.md)).
"V1" is the first-release scope ([AGENTS.md](../../AGENTS.md#non-negotiables), item 8). The `net`
module is an app-framework module ([ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md))
and the transport under [`async-data-v1.md`](async-data-v1.md), which owns cache keys, retry policy
and UI state. Rationale and options: [`app-framework-open-questions.md`](../design/app-framework-open-questions.md)
section 5.

Terms: **back end** = the code behind the `net` interface for one platform stack. **Contract suite** =
the shared test set every back end must pass. **Completion queue** = `runtime::CompletionQueue`
([`async.md`](../design/async.md)).

## 1. Goals and non-goals

Goals: HTTPS requests that never block the UI thread; deterministic tests with no network; the OS
trust store, proxy and security policy; one behavior across back ends.

Non-goals in V1: WebSocket and streaming upload (a separate interface later), HTTP/3 as a
requirement, certificate pinning, an implicit cookie jar or HTTP cache, raw sockets, and any
platform type in the interface.

## 2. Interface

The interface is shaped like `fetch`: a request, a buffered or streamed body, cancellation and a
timeout. It exposes no sockets and no platform handles.

| Type | Fields |
|---|---|
| `Request` | `method` (`GET`, `HEAD`, `POST`, `PUT`, `PATCH`, `DELETE`), `url` (absolute `https` or `http`), `headers` (ordered name/value list), `body` (bytes, optional), `timeout` (per-attempt, milliseconds, default 30000), `max_response_bytes` (default 16 MiB), `redirect` (`follow` or `manual`, default `follow`) |
| `Response` | `status` (int), `headers`, `body` (bytes, or a stream when `Request.stream` is set), `final_url`, `protocol` (`h1`, `h2`, `h3`, `unknown`) |
| `NetError` | `kind`, `message`, `os_code` (optional, diagnostic only) |

`NetError.kind` is one of: `cancelled`, `timeout`, `offline`, `dns`, `connect`, `tls`,
`too_large`, `too_many_redirects`, `bad_url`, `unsupported`, `other`. A `Response` with a 4xx or
5xx `status` is **not** an error at this layer; the caller decides ([`async-data-v1.md`](async-data-v1.md)
maps statuses to retry rules).

`send(Request, CancelToken) -> Task<std::expected<Response, NetError>>`. The result is delivered
through the completion queue on the UI thread, in submission order per request. The call itself
returns at once.

## 3. Normative behavior (what the contract suite pins)

1. **No blocking:** `send` never waits on the network on the calling thread. Back ends marshal every
   platform callback (WinHTTP and NSURLSession call back on their own threads) through the completion
   queue; app code never runs on a platform thread.
2. **Redirects:** at most 10 with `follow`; the 11th is `too_many_redirects`. `301`, `302` and `303`
   change `POST` to `GET` and drop the body; `307` and `308` keep method and body. An
   `Authorization` header is dropped when a redirect changes the origin. An `https` to `http`
   redirect is refused with `unsupported`.
3. **Timeout:** per attempt, measured from the start of the attempt to the last body byte, using the
   injected clock; it yields `timeout`. There is no implicit retry.
4. **Cancellation:** cancelling before completion yields `cancelled` exactly once, within one loop
   tick of the back end observing it, and releases the connection; cancelling after completion does
   nothing. A cancelled request never delivers a body.
5. **Size limit:** a body larger than `max_response_bytes` yields `too_large` and discards the
   data read so far.
6. **Offline:** when the platform reports no connectivity the result is `offline`, not `connect`,
   if the platform can tell the two apart; otherwise `connect`.
7. **Headers:** names compare case-insensitively and are stored as sent; the library adds no
   `User-Agent` beyond a fixed `Tessera/<version>` and sets no cookie, cache or `Accept-Encoding`
   header the caller did not ask for (a back end may negotiate compression and must hand back the
   decoded body).
8. **TLS:** certificate and host validation always on, using the OS trust store; no API to disable
   it. A validation failure is `tls`. Plain `http` is allowed only when the app enables it (a
   `net.allowCleartext` project setting, default `false`); iOS App Transport Security and Android
   cleartext policy apply on top.
9. **Redaction:** URLs in diagnostics drop the query string and userinfo by default; `Authorization`
   and `Cookie` header values never appear in logs ([`diagnostics.md`](../design/diagnostics.md)
   redaction rules).
10. **Determinism:** no wall clock, random id or system proxy read inside the interface layer; all
    time comes from the injected `Clock`.

## 4. Back ends and order

| Order | Back end | Notes |
|---|---|---|
| 1 | `FakeNet` (in-tree) | Deterministic scripted responses, delays on the manual clock, injected failures; the default in tests and headless runs |
| 2 | WinHTTP (Windows) | Async mode, callbacks marshalled to the completion queue |
| 3 | libcurl (Linux) | Multi interface with the distribution CA bundle; TLS library chosen in that slice (mbedTLS or OpenSSL, both Apache-2.0); needs `dependencies.md` and `THIRD_PARTY_NOTICES.md` entries before it lands |
| 4 | Browser `fetch` (Wasm) | CORS applies; no raw sockets; `redirect: manual` may be unsupported (`unsupported`) |
| 5 | NSURLSession (Apple) | macOS and iOS |
| 6 | Android | With the Android backend; HttpURLConnection or Cronet through JNI |

A back end may be unavailable in a build; `send` then yields `unsupported`. Community back ends
(for example one libcurl everywhere) plug in behind the same interface as Tier 2 providers.

## 5. Tests

- The contract suite runs against `FakeNet` in normal CI on every platform.
- Each real back end runs the same suite against an in-process loopback server in an opt-in lane
  (`TESSERA_NET_LOOPBACK_TESTS=ON`); no test touches the external network.
- Required cases: every item of section 3, a `5xx` response is a `Response`, cancellation during a
  streamed body, a timeout with the manual clock, a redirect loop, a cross-origin redirect dropping
  `Authorization`, an offline platform state, and "UI thread never blocks" (a frame tick completes
  while a request is pending).
- A static scene with a pending request still idles with zero presents.

## 6. Versioning

The interface is part of the public surface ([`versioning-v1.md`](versioning-v1.md)); adding an error
kind or an optional `Request` field is a minor change, changing a normative rule of section 3 is a
major change. The loopback suite is the compatibility check for back ends.

## 7. Open questions

- WebSocket and server-sent events (a separate interface, after V1).
- Response streaming to disk for large downloads (`IoExecutor` hand-off).
- Whether HTTP/3 is requested explicitly or left to the platform (V1: left to the platform).
