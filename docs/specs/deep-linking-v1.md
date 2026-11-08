# Deep linking and external activation v1

Status: proposed (Big Step F). Route matching/stack/guards:
[`navigation-v1.md`](navigation-v1.md). Decision: ADR-026. Companion to
[`startup-and-loading.md`](../design/startup-and-loading.md),
[`services.md`](../design/services.md), and [`security.md`](../design/security.md).

Opening a link/file must activate the app, route to a screen, and focus the right window. The
link is **untrusted input** and must be declared/validated by the developer, not hardcoded in
platform files.

Canonical route ownership: `ui/routes.json` (`navigation.routesFile`) defines screens, params,
and guards. `deepLinks.routes` below only maps an external pattern to an existing route id; it
must not declare screens or duplicate the route table.

## 1. Activation sources

| Source | Example | Notes |
|---|---|---|
| Custom URL scheme | `myapp://orders/42` | Desktop/browser prompt; can be spoofed |
| HTTPS App/Universal Link | `https://app.example.com/orders/42` | Verified by OS; preferred on mobile |
| File association | open `report.myapp` | Path/URI validation, sandbox |
| Command line | `myapp --url ...` | Cold start; parse before UI |
| Notification action | tap notification | Payload validation |
| Share/open-with | another app shares a link | Same normalized event |
| Single-instance forward | second launch | Focus existing window |

Rule: all sources normalize to `DeepLinkEvent`; the UI never receives raw platform intents.

## 2. Normalized event

```cpp
enum class ActivationSource { Url, File, CommandLine, Notification, Share, Other };

struct DeepLinkEvent {
    std::string url;                 // validated/normalized
    ActivationSource source;
    bool cold_start = false;
    std::optional<WindowId> target_window;
    std::string route;               // resolved canonical route
    std::map<std::string, std::string> params;
    std::int64_t timestamp_ns = 0;
};
```

Lifecycle:

```text
OS activation -> platform adapter -> validate/allowlist -> queue
  -> app not ready? keep pending (bounded)
  -> auth required? keep pending until auth succeeds
  -> resolve route -> focus/create window -> navigate
  -> dedupe duplicate activations
```

- Pending activation is bounded (latest wins or a small queue); never an unbounded backlog.
- Cold start: parse before first screen; route after shell/auth.
- Warm start: route immediately; focus existing window unless `openIn=newWindow`.
- Duplicate activation (same URL within a short window) is ignored.
- Unknown route -> configured home/error route; malformed -> error, not crash.

## 3. Developer declaration

`tessera.json`:

```json
"deepLinks": {
  "schemes": ["myapp"],
  "hosts": ["app.example.com"],
  "routes": [
    { "pattern": "/orders/:id", "route": "orders.detail", "auth": true },
    { "pattern": "/settings", "route": "settings", "auth": true },
    { "pattern": "/share", "route": "share", "auth": false, "confirm": true }
  ],
  "fileTypes": ["myapp"],
  "singleInstance": true,
  "openIn": "existing",
  "allowColdStart": true,
  "fallbackUrl": "https://app.example.com"
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `schemes` | string[] | `[]` | Custom URL schemes; lowercase, `[a-z][a-z0-9+.-]*` |
| `hosts` | string[] | `[]` | HTTPS hosts for App/Universal Links |
| `routes` | Route[] | `[]` | Pattern -> canonical route + auth/confirm |
| `fileTypes` | string[] | `[]` | Extensions/UTIs/MIME handled by the app |
| `singleInstance` | boolean | `true` | Forward activation to existing instance |
| `openIn` | string | `existing` | `existing` or `newWindow` |
| `allowColdStart` | boolean | `true` | Allow launch via link |
| `fallbackUrl` | string | — | HTTPS web fallback when the app is absent |
| `maxUrlLength` | integer | `4096` | Hard cap before parsing |

Rules:

- `schemes`/`hosts` are allowlists; no wildcard hosts.
- Route patterns are unique and resolvable to a real route; `:param` captures a path segment.
- `auth=true` means the route waits for auth; the pending link survives login.
- `confirm=true` means destructive/privileged actions require a user confirmation.
- `fallbackUrl` must be HTTPS; the web page should offer an "Open app" action.
- Unknown fields/duplicate patterns are schema errors.

## 4. Platform declarations

The generator validates `deepLinks` and emits or checks platform declarations where supported;
manual files must match the schema.

### Windows

- Unpackaged Win32: registry `HKCU\Software\Classes\<scheme>` with `URL Protocol` and
  `shell\open\command = "app.exe" "%1"`.
- MSIX: `uap:Protocol` / `uap:FileTypeAssociation` extensions in the manifest.
- Command line/activation payload parsed at startup; single-instance via named mutex/pipe or
  `AppInstance` redirection; `SetForegroundWindow` for focus.
- File associations via `OpenWithProgids`/`shell\open\command` or MSIX file-type association.

### macOS

- `CFBundleURLTypes`/`CFBundleURLSchemes` for custom schemes.
- Universal Links: `com.apple.developer.associated-domains` (`applinks:app.example.com`) +
  hosted `apple-app-site-association`; handle in `UISceneDelegate`/`NSApplicationDelegate`.
- File types: `CFBundleDocumentTypes`/`UTImportedTypeDeclarations`.
- Single instance is the app model; focus the existing window and route.

### Linux

- `.desktop` file:
  `MimeType=x-scheme-handler/myapp;` and `Exec=/path/myapp %u`;
  register with `xdg-mime default myapp.desktop x-scheme-handler/myapp` and
  `update-desktop-database`.
- File types via `MimeType=application/x-myapp` and `%f`.
- Single-instance via D-Bus (`org.freedesktop.Application` `Open`/`Activate`) or a named bus.
- `xdg-open`/`gio open` for testing.

### Android

- Custom scheme intent-filter (`scheme=myapp`) and/or HTTPS App Links:
  `autoVerify=true`, `host=app.example.com`, `BROWSABLE`/`DEFAULT`.
- Host `assetlinks.json`; verify with `adb shell pm get-app-links`.
- `launchMode=singleTask` + `onNewIntent` for warm activation; `exported=true` only for the
  intent filter.
- File/content associations via MIME/pathPattern/content URI; validate URIs and permissions.

### iOS/iPadOS

- `CFBundleURLTypes` for custom schemes.
- Universal Links entitlement + `apple-app-site-association`; handle
  `scene(_:openURLContexts:)`/`scene(_:continue:)`.
- File types: `CFBundleDocumentTypes`/`UTImportedTypeDeclarations`.
- Scene activation decides whether to reuse or open a scene/window.

## 5. Routing

- Match host/scheme and path pattern; percent-decode path segments once.
- Reject oversized URLs, control characters, encoded separators/traversal.
- Query parameters are untrusted strings; validate type/range/length per route.
- Canonicalize to a route + typed params; do not pass raw URL into UI code.
- `auth=true` routes wait for auth; the pending link is resumed after login.
- `confirm=true` routes show a confirmation before destructive actions.
- Unknown route -> home/error; don't silently ignore.
- Deep links never carry access tokens/secrets; use a one-time code if needed.

## 6. Single instance and windows

- `singleInstance=true` (default): forward activation to the existing process, focus the
  window, route.
- `openIn=existing`: reuse/focus the main window.
- `openIn=newWindow`: create a new window with the route (if the app supports multi-window).
- If no window exists (cold start), create the main window and route.
- Deduplicate identical activations within a short window.
- Do not steal focus without a user-driven activation; platform focus rules apply.

## 7. Security

- Custom schemes can be claimed by other apps: treat them as untrusted.
- App/Universal Links are OS-verified, but their params are still untrusted.
- Allowlist scheme/host/route; no wildcard hosts; no open redirect.
- Max URL length; reject malformed/oversized/percent-encoded traversal/control chars.
- Never shell-execute a URL/path; use OS APIs with parsed values.
- File paths: canonicalize, respect sandbox/security-scoped bookmarks, reject traversal.
- No secrets/tokens in URLs; redact query strings in logs/crash dumps.
- Destructive actions require confirmation; no auto-execution from a link.
- Rate-limit/dedupe activations; log activation failures without sensitive payloads.

## 8. Failure and fallback

- Unknown/invalid route: error/home route; diagnostic; no crash.
- App not installed: browser shows `fallbackUrl`; the web page offers "Open app" (user gesture).
- Browser may prompt before opening a custom scheme; don't assume auto-launch.
- Offline: route to cached/offline shell; retry when online.
- Auth failure/cancel: keep or discard the pending link explicitly; never leak route params.
- Logout: clear pending user-scoped links and cached route state.
- Multiple links: latest wins or bounded queue; define per app.

## 9. Testing

- Platform commands:
  - Windows: `start myapp://orders/42`
  - macOS: `open myapp://orders/42`
  - Linux: `xdg-open myapp://orders/42`
  - Android: `adb shell am start -a android.intent.action.VIEW -d "myapp://orders/42"`
  - iOS: `xcrun simctl openurl booted "myapp://orders/42"`
- Cold start vs warm start; single-instance forwarding/focus.
- Auth-required link: pending until login; resume; logout cleanup.
- Malformed/oversized/encoded traversal; unknown route; duplicate activation.
- File association/open-with; sandbox/permission denied.
- Browser fallback: app installed/not installed; no auto-launch assumptions.
- Fuzz the URL parser and route matcher.
- No sensitive data in logs; redaction tests.

## 10. Open questions

- Default `openIn` per app profile (existing vs new window).
- Pending queue policy (latest vs bounded queue).
- One-time codes for auth links vs no secrets in URLs.
- Associated-domain/assetlinks generation in the CLI.
- Windows packaged vs unpackaged declaration parity.
- Linux desktop environment differences (GNOME/KDE/xdg-open).
- How to present "open app" on web fallback without user-agent sniffing.
- Deep link to a notification action with sensitive params.
