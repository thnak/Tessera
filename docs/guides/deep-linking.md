# Deep linking guide

Status: proposed (F). Companion to
[`deep-linking-v1.md`](../specs/deep-linking-v1.md) and
[`project-config-v1.md`](../specs/project-config-v1.md).

Use this to let `myapp://...` or `https://app.example.com/...` open/focus the app and navigate.

## 1. Declare in `tessera.json`

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

- `schemes`: custom URL schemes.
- `hosts`: HTTPS hosts for App/Universal Links.
- `routes`: pattern -> canonical route, `auth`, `confirm`.
- `fileTypes`: file associations.
- `singleInstance`: focus existing process by default.
- `fallbackUrl`: web page when the app is not installed.

## 2. Platform setup

### Windows

Unpackaged Win32 registry (per-user):

```text
HKCU\Software\Classes\myapp
  (Default) = "URL:myapp"
  URL Protocol = ""
HKCU\Software\Classes\myapp\shell\open\command
  (Default) = "\"C:\Path\MyApp.exe\" \"%1\""
```

Packaged MSIX: add `uap:Protocol`/`uap:FileTypeAssociation` to the manifest. Parse the
activation/command line at startup; forward to the existing instance (named mutex/pipe or
`AppInstance`) and focus its window.

Test:

```powershell
start myapp://orders/42
```

Chrome may show an "Open app?" prompt; do not assume silent launch.

### macOS

`Info.plist`:

```xml
<key>CFBundleURLTypes</key>
<array>
  <dict>
    <key>CFBundleURLName</key><string>com.example.myapp</string>
    <key>CFBundleURLSchemes</key><array><string>myapp</string></array>
  </dict>
</array>
```

Universal Links: add `com.apple.developer.associated-domains` (`applinks:app.example.com`) and
host `apple-app-site-association`; handle `scene(_:openURLContexts:)`/`scene(_:continue:)`.
File types: `CFBundleDocumentTypes`.

Test:

```bash
open "myapp://orders/42"
```

### Linux

`myapp.desktop`:

```ini
[Desktop Entry]
Name=MyApp
Exec=/path/myapp %u
Type=Application
MimeType=x-scheme-handler/myapp;
```

Register and test:

```bash
xdg-mime default myapp.desktop x-scheme-handler/myapp
update-desktop-database ~/.local/share/applications
xdg-open "myapp://orders/42"
```

For single instance, forward via D-Bus (`org.freedesktop.Application` `Open`/`Activate`) and
focus the existing window.

### Android

`AndroidManifest.xml`:

```xml
<intent-filter android:autoVerify="true">
  <action android:name="android.intent.action.VIEW" />
  <category android:name="android.intent.category.DEFAULT" />
  <category android:name="android.intent.category.BROWSABLE" />
  <data android:scheme="https" android:host="app.example.com" />
</intent-filter>
<intent-filter>
  <action android:name="android.intent.action.VIEW" />
  <category android:name="android.intent.category.DEFAULT" />
  <category android:name="android.intent.category.BROWSABLE" />
  <data android:scheme="myapp" />
</intent-filter>
```

- Host `https://app.example.com/.well-known/assetlinks.json`.
- `launchMode="singleTask"` + `onNewIntent` for warm activation.
- Verify: `adb shell pm get-app-links com.example.myapp`.
- Test: `adb shell am start -a android.intent.action.VIEW -d "myapp://orders/42"`.

### iOS/iPadOS

`Info.plist` URL types as above; Universal Links entitlement + AASA; handle scene URL contexts.
Test:

```bash
xcrun simctl openurl booted "myapp://orders/42"
```

## 3. Web fallback

- Canonical share link should be `https://app.example.com/...`.
- If the app is installed, the OS may open it (App/Universal Links on mobile).
- Otherwise show the web page with an "Open app" button using `myapp://...`.
- Custom schemes often require a user gesture; don't auto-launch.
- Provide a copyable link and a download/store link.
- Never put tokens/secrets in the URL.

## 4. Route mapping

- `:id` captures one path segment; no wildcard hosts.
- Percent-decode once; reject encoded traversal/control chars.
- Validate query params (type/range/length) per route.
- Unknown route -> home/error, not a crash.
- `auth=true` -> pending until login; `confirm=true` -> ask before destructive action.

## 5. Single instance

- Default: focus the existing window and route.
- `openIn=newWindow`: create a new window (if supported).
- Cold start: create main window, route after shell/auth.
- Deduplicate identical activations; don't open five windows for one click.

## 6. Testing checklist

- [ ] cold start from link
- [ ] warm start / existing window focus
- [ ] single-instance forwarding
- [ ] auth-required link (pending -> login -> route)
- [ ] logout clears pending user-scoped link
- [ ] unknown/malformed/oversized/encoded-traversal URL
- [ ] duplicate activation
- [ ] file association/open-with
- [ ] browser fallback with app installed/not installed
- [ ] no secrets in logs/crash dumps
- [ ] web fallback page has an explicit "Open app" action

## 7. Common mistakes

- Hardcoding platform manifests that drift from `tessera.json`.
- Assuming Chrome/Safari will open a custom scheme silently.
- Using a custom scheme as the only link on mobile.
- Passing raw URL/path to a shell.
- Putting auth tokens in the URL.
- Auto-executing destructive actions from a link.
- Opening a new window for every activation.
- Forgetting to resume the pending link after auth.
- Logging full query strings with secrets.

## 8. Status

- Spec/guide only; not implemented.
- The generator should validate `deepLinks` and emit/check platform declarations where possible.
- Test on real browsers/devices; emulator/simulator behavior differs.
