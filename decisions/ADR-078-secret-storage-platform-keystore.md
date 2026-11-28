# ADR-078: Secret storage: a SecretStore over the platform keystore, no insecure fallback

- Status: Proposed (2026-11-28). Spike evidence landed (fake store, Windows Credential Manager);
  nothing is Accepted and no platform back end beyond the Windows spike exists.
- Date: 2026-11-28
- Related: [security.md](../docs/design/security.md) (section 6 requires secrets in the platform
  keystore; section 11 lists this as open), [ADR-012](ADR-012-static-linking.md) (static linking, no
  LGPL), [ADR-021](ADR-021-async-runtime.md) (never block the UI thread),
  [ADR-050](ADR-050/README.md) (services, layer 1 modules),
  [ADR-052](ADR-052-networking-platform-stacks.md) (the HTTP back ends that send tokens),
  [ADR-025](ADR-025-error-boundaries.md) (errors do not cross boundaries as exceptions). Issues:
  #841 (this), #411 (the broader storage item it was split from), #403 (crash-report redaction gate),
  #835 and #836 (tokens sent by the real HTTP back ends).
- Terms: a **keystore** is the OS facility that encrypts a secret under the user's login (Credential
  Manager, Keychain, Secret Service, Android Keystore). A **secret** is a short byte string: an API
  token, a refresh token, a database key.

## Context

Apps that call authenticated APIs need somewhere safe to keep tokens. Today there is none, so an app
puts a token in `storage` (a plain file) or in config. security.md section 6 forbids both and names the
keystore without saying how an app reaches it. This ADR decides the model, the API and the platform
mapping so the implementation issues can be cut.

Facts that bound the design:

- The engine speaks the D-Bus wire protocol directly (`include/tessera/dbus`: address, SASL
  EXTERNAL auth, transport, wire marshalling) for the AT-SPI2 bridge, precisely so it does not link
  glib or libatspi (ADR-012). The Linux Secret Service is a D-Bus API, so the same code can reach it.
- `libsecret` is LGPL and drags glib; ADR-012 forbids linking it.
- A keystore call can block (a locked keyring prompts the user, Keychain may show a dialog), so it can
  never run on the UI thread.
- Wasm has no secret store. Browser storage is readable by any script on the origin.

## Decision

### 1. Threat model

| Protects against | Yes | Notes |
|---|---|---|
| Another user on the machine | Yes | per-user keystores; the entry is not readable by other accounts |
| A stolen disk or a backup | Yes on Windows/macOS/iOS/Android | the secret is encrypted under the login credential or hardware; Linux depends on the keyring being locked at rest |
| Secrets in config, logs, crash reports, saved UI state | Yes | the API and diagnostics rules in section 4 |
| Malware running as the same user | **No** | on desktop, any process of the user can ask the keystore; no keystore here stops that. We say so; we do not claim it |
| A compromised app process | **No** | the secret is in memory while used |
| Network capture | Out of scope | TLS (ADR-052) |

The assets are tokens and refresh tokens, and the key that encrypts a local database. Secrets are
small (a few hundred bytes; section 5 covers the size limit).

### 2. The seam: a blocking back end, an async store

`SecretBackend` is the platform seam: blocking `get`/`set`/`erase`, an `available()` probe and a
`max_value_bytes()`. It is called only on an executor thread and names no platform type. A back end
is one translation unit per platform behind it. `SecretStore` is what an app holds: it validates, then
posts one blocking body to the `IoTaskExecutor` and delivers a `std::expected<..., SecretError>` on
the draining (UI) thread, with a `CancellationToken`, exactly the `Storage` pattern. `post()` never
runs the body inline.

```cpp
store.get({"api.example.com", "refresh-token"}, token,
          [](std::expected<SecretBytes, SecretError> r) { /* UI thread */ });
```

A `SecretKey` is `{service, account}`, each 1 to 256 bytes with no control characters. Erase of an
absent entry succeeds. The error type is a code (`NotFound`, `Unavailable`, `AccessDenied`,
`InvalidKey`, `TooLarge`, `Cancelled`, `Busy`, `Backend`), a literal step name and a native code.

The service form (`useService<SecretStore>`, ADR-050) wraps the same object; the TSX surface, if any,
is a separate ADR and must not expose the value as a prop or state (it would land in saved UI state and
snapshots).

### 3. No insecure fallback

If the keystore is unavailable, `set` fails with `Unavailable`. There is no plaintext file, no
obfuscated file and no environment-variable fallback "for convenience", and none in the library at all:
a fallback is a policy the app must choose and see, not one the framework picks silently. `get` and
`erase` report `Unavailable` the same way. An app that wants a session-only secret holds it in memory
itself.

### 4. Secrets never reach logs

- `SecretBytes` is move-only, zeroed on destruction, and has no conversion to a string view or
  stream. Its only printable form is `describe()`: `<secret N bytes>`.
- `SecretError` carries no key and no value; `describe_secret_error` prints code and step only.
- A back end's call record (the fake's `calls()`) holds operation and key, never a value.
- Redaction tests extend the #403 gate: no test log line, error text or crash payload produced around a
  secret operation contains the value (the spike's `appfw_secret` test is the template). Header
  redaction of `Authorization` in net diagnostics is the net module's own gate (#835/#836).
- A back end must wipe the OS copy it holds after copying (the Windows spike does).

### 5. Platform mapping

| Platform | Back end | Notes |
|---|---|---|
| Windows | **Credential Manager** (`CredWriteW`/`CredReadW`/`CredDeleteW`, `CRED_TYPE_GENERIC`, `CRED_PERSIST_LOCAL_MACHINE`, target `tessera:<service>/<account>`) | Per user, visible in the OS UI, managed by the OS. Limit 2560 bytes per blob. Not `ENTERPRISE` (roams to a domain). Spike landed. |
| Windows alt | DPAPI (`CryptProtectData`) | Rejected as the default: it returns a blob the app must store somewhere, which reintroduces a file and its lifecycle. Kept as the encryption half of the wrapped-key form below. |
| Linux | **Secret Service over D-Bus**, spoken with `tessera::dbus` (no libsecret, no glib) | Needs a client for `org.freedesktop.secrets`: open a session (`plain` or the DH algorithm), `SearchItems`, `CreateItem`, `GetSecret`, `Delete`, unlock prompts. Item attributes carry `service`/`account`. No service on the bus (headless, minimal WMs) is `Unavailable`. |
| macOS, iOS | Keychain (`SecItemAdd`/`CopyMatching`/`Delete`, `kSecClassGenericPassword`, `kSecAttrAccessibleWhenUnlockedThisDeviceOnly`) | Post-V1. `ThisDeviceOnly` keeps it out of backups. |
| Android | Android Keystore wrapping key | Post-V1. The Keystore holds keys, not arbitrary bytes: a non-exportable AES-GCM key encrypts the secret and the ciphertext goes in app-private storage. |
| Wasm | **None** | `available()` is false; every call is `Unavailable`. Never `localStorage`, `IndexedDB` or a cookie: any script on the origin reads them. |

**Large secrets.** A secret over the keystore's cap (Credential Manager 2560 bytes) uses the
**wrapped-key form**: a random 256-bit key goes in the keystore and the payload is AES-GCM encrypted
into `storage`. This is post-V1 and is the same shape Android needs, so it is built once. The store
refuses an oversized `set` with `TooLarge` until then.

### 6. The fake

`FakeSecretBackend` is an in-memory double in the module (like `FakePermissionBackend`): deterministic,
`push_failure(code)` for one-shot `AccessDenied`/`Backend` results, `set_available(false)` for the
no-keystore case, `set_max_value_bytes`, and a call log without values. It is named as a double, is
never selected by the library, and an app test passes it explicitly. The issue asked for it in
`tessera_testing`; it lives next to the seam instead for the reason `FakeNet` and `FakeShell` do (a
headless run and the Studio emulator share one definition).

## Consequences

- Apps get one supported place for a token; the "token in a plain file" excuse ends.
- Linux needs real work (a Secret Service client on `tessera::dbus`, with prompts), but no new
  dependency and no LGPL link. The cost is protocol code we own.
- Same-user malware is explicitly out of scope; docs say so.
- Credential Manager's 2560-byte limit forces the wrapped-key form for anything larger.

## Alternatives considered

- **libsecret on Linux.** LGPL, glib; rejected by ADR-012.
- **An encrypted file with an app-managed passphrase as the Linux fallback.** A silent fallback
  hides the weaker guarantee. If wanted, it is an explicit opt-in API added by its own ADR.
- **DPAPI only on Windows.** A blob in a file we manage; Credential Manager is the managed store.
- **One cross-platform encrypted file for all platforms.** Reinvents the keystore, and the key has to
  live somewhere.
- **Wasm fallback to browser storage.** Not a secret store (XSS reads it).

## Evidence required before Accepted

1. Windows: a Credential Manager round trip, erase, overwrite, absent key. **Done** (spike:
   `appfw_secret_wincred`, real vault, unique key, erased after). Open: behavior under a different
   logon (service accounts, CI with no profile).
2. The fake and the async contract with redaction checks. **Done** (`appfw_secret`: round trip,
   NotFound, Unavailable with nothing stored, scripted AccessDenied, cancellation before the body,
   key validation, size caps, executor-full refusal, no value in error text or log lines).
3. Linux: a Secret Service `CreateItem`/`GetSecret` round trip on `tessera::dbus` against
   gnome-keyring or KWallet in CI, including the locked-collection prompt, and the "no service on the
   bus" path reporting `Unavailable`. **Open.**
4. Crash-report gate: a crash raised during a secret operation produces a payload with no value
   (`contains_unredacted` extended to the `<secret` marker is not needed: the value is never
   formatted; the test proves it). **Open.**
5. The wrapped-key form: payload encrypt/decrypt with a keystore-held key, size limits. **Open.**

## Follow-up

- Implementation issues: Windows back end hardening (service-account logon, #841 follow-up), Linux
  Secret Service client, wrapped-key form, macOS/iOS and Android back ends (post-V1).
- Narrow #411 to its remaining items once this is Accepted.
- Wire `SecretStore` into the `HttpService` auth hooks (#835/#836) so a token is read at send time and
  never stored on a request object that is logged.
- Remove the security.md section 11 open question when Accepted.
