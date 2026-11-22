# Permissions v1

Status: draft, normative once accepted. This is the one document that owns the permission model
(issue #451). Until now permissions were mentioned in five places and owned by none:
[`platform-modules.md`](../design/services/platform-modules.md) sections 6 and 8 ("request only
what is needed", "tests: permission denied"), [`platform-shell-v1.md`](platform-shell-v1.md)
`ShellCapabilities::permissioned`, [`mobile-integration.md`](../design/mobile-integration.md)
section 3.7 (the manifest declarations), [`security.md`](../design/security.md) and
[`media-components-v1.md`](media-components-v1.md) (`permission_denied`). Each described a corner;
none described the state machine, and a feature could not be written against any of them.

This spec fixes three things and deliberately leaves a fourth:

1. the **state model** (section 3) and the rule that denial is a value, not an error;
2. the **API**: one `request` and one `status` per permission, with rationale text the app supplies
   (sections 4 and 5);
3. the **declaration** rule: the project config lists what the app may ask for, and code that asks
   for anything else fails the build (section 7).

It does not fix the platform call sequences. Section 8 maps each platform to what it does with each
permission, and section 11 lists what remains open.

## 1. Why this is a spec and not an implementation detail

A permission is the only interaction where the application's *honesty* is observable by the user and
by the store. Three consequences drive every rule below:

- **The OS remembers.** A denial on Android or iOS is not a dialogue you can reopen; it is a
  persistent bit, and on iOS a "permanently denied" permission can only be changed in Settings. An
  app that treats denial as a transient failure will show a broken screen forever.
- **The store audits declarations.** A permission used without a matching manifest declaration is
  a crash on Android and a rejection at review on both stores. The declaration is therefore a
  *build* input, not a runtime string.
- **The prompt is a scarce resource.** Security research is consistent that repeated prompts train
  users to accept without reading (the "permission fatigue" effect). The rules in section 5 exist
  to make the module, not the app author, count the prompts.

## 2. Terms

| Term | Meaning |
|---|---|
| **permission** | one named capability the OS gates, from the closed set of section 3 |
| **state** | what the OS currently says about one permission, section 4 |
| **rationale** | the app's own sentence explaining why, shown *before* the OS prompt |
| **just-in-time request** | a request made when the user first reaches the feature, not at launch |
| **seam** | `hal::PermissionBackend` (`include/tessera/hal/permission.hpp`), the only platform contact |
| **declared** | listed under `permissions` in `tessera.json` |

## 3. The permission set

The set is closed for V1. A permission not in this table does not exist, which is what keeps the
module from becoming a stringly-typed pass-through to each OS.

| Permission | Id | Android | iOS | Windows | macOS | Desktop Linux |
|---|---|---|---|---|---|---|
| Camera | `camera` | `CAMERA` | `NSCameraUsageDescription` | capability prompt | usage string | none |
| Microphone | `microphone` | `RECORD_AUDIO` | `NSMicrophoneUsageDescription` | capability prompt | usage string | none |
| Location | `location` | `ACCESS_FINE_LOCATION` | `NSLocationWhenInUseUsageDescription` | location prompt | usage string | none (GeoClue, no prompt) |
| Notifications | `notifications` | `POST_NOTIFICATIONS` (13+) | `UNUserNotificationCenter` request | none | request | none |
| Photo library | `photo-library` | `READ_MEDIA_IMAGES` (13+) | `NSPhotoLibraryUsageDescription` | none | usage string | none |
| File access | `file-access` | scoped storage | document picker, no prompt | `FileOpenPicker`, no prompt | `NSOpenPanel`, no prompt | `xdg-desktop-portal`, no prompt |

Notes that are part of the contract:

- **`none` means the request resolves `Granted` without a prompt.** A desktop Linux camera call
  goes through PipeWire and asks the *compositor*; it does not go through this module. So
  `request(camera)` on that platform is a no-op that resolves `Granted`, and the app's code path is
  the same on every platform.
- **File access is not a permission on four of five platforms** — it is a picker. It appears in the
  set because the *user-visible question* is the same ("may this app see my files?"), and an app
  that asks before opening the picker gives better copy than one that does not. The module reports
  what the platform reports and never invents a prompt the platform does not have.
- **Notifications is the only permission whose denial a well-built app can survive silently**, which
  is why `media-components-v1.md` and `platform-modules.md` both name it first.

## 4. The state model

```text
                    +----------> NotDetermined <-----------+
                    |                 |                     |
                    |          request()                    |
                    |                 |                     |
                    |        +--------+--------+            |
                    |        |                 |            |
                    v        v                 v            |
              Granted   Denied         PermanentlyDenied    |
                    |        |                 |            |
                    |        +-----------------+            |
                    |                                          |
                    +---------- Restricted --------------------+
```

Five states, and each one is a *different thing for the app to do*:

| State | Meaning | What the app must do |
|---|---|---|
| `NotDetermined` | never asked (or the OS has no answer) | ask |
| `Granted` | allowed now | use the feature |
| `Denied` | refused this time; the OS will ask again | explain, offer a retry |
| `PermanentlyDenied` | refused; the OS will not ask again | explain, offer Settings |
| `Restricted` | blocked by policy the user cannot change (parental controls, MDM, an enterprise profile) | hide the feature, explain |

The distinctions that matter, stated explicitly because conflating any pair is the bug this model
exists to prevent:

1. **`Denied` is not `PermanentlyDenied`.** Re-requesting a `Denied` permission shows a prompt;
   re-requesting a `PermanentlyDenied` one does nothing and returns `PermanentlyDenied` forever. An
   app that treats them alike either nags a user it cannot persuade, or gives up on one it could.
2. **`Restricted` is not a denial.** The user did not refuse; a policy did. Offering "try again" is
   dishonest, and offering "open Settings" leads to a switch that is greyed out.
3. **`Denied` is a value, not an error.** A request that the user refuses returns a state, not a
   `PermissionError`. Errors are for the module failing (section 6), which is a different thing to
   report and a different thing to retry.

The state is **per permission**, and there is no aggregate "all permissions". A camera-and-mic app
can have one of each in different states, and a combined prompt is a platform decision (§8), never
one the module invents.

## 5. The request flow

One call, one result:

```cpp
// Returns immediately. The answer arrives on the completion queue (the UI thread).
PermissionTicket request(Permission permission, PermissionRequestOptions options,
                         PermissionCallback on_done);
```

`PermissionRequestOptions`:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `rationale` | `string` | empty | the app's sentence, shown before the OS prompt |
| `retry_after_denial` | `bool` | `true` | whether a `Denied` permission may be asked again |

### The rules

Each rule is normative and has a test.

1. **A request is asynchronous and never blocks.** It returns a ticket at once and completes on the
   injected `CompletionQueue`. No rule in this spec permits a synchronous prompt, because a prompt
   on the UI thread is a deadlock on Android.
2. **A granted permission resolves without a prompt.** Asking for what you already have is a
   `Granted` result and zero platform calls. The module checks before it asks.
3. **A `PermanentlyDenied` permission never prompts again.** The request resolves
   `PermanentlyDenied` and the platform is not called. This is rule 1 of section 4 made operational.
4. **A `Restricted` permission never prompts.** Same shape: the result is `Restricted`, no call.
5. **A second request while one is pending coalesces.** It does not prompt twice and it does not
   return `Busy`; both callers get the one answer. This is the acceptance criterion the issue names
   ("coalesces, never prompts twice"), and the reason the ticket is a value rather than a
   callback-only api.
6. **The rationale is the app's text and the module never writes prose.** An empty rationale is
   legal and means "the platform's own copy"; a non-empty one is passed through unmodified. The
   module has no strings to localise, which is why it needs no catalogue.
7. **A `Denied` permission asked again with `retry_after_denial = false` resolves `Denied` without a
   prompt.** An app that has shown its own "not now" screen opts out of the OS prompt; the module
   does not count retries itself.
8. **The state is updated by the result, and only by the result.** A request that was cancelled, or
   whose platform call failed, leaves the state where it was. The module never guesses that a prompt
   was shown.
9. **A result that arrives after the service is destroyed is dropped.** The callback is not invoked;
   the ticket's record is gone. The token-generation check is the same one `net` and `shell` use.
10. **The module records one diagnostic per refusal**, never one per call, so an app that asks in a
    loop cannot flood the diagnostics ring (ADR-022's "values and counters, not a log line per
    call").

## 6. Errors

`PermissionError` is for the module failing, not the user refusing:

| Kind | Meaning |
|---|---|
| `Unsupported` | this build or platform has no such permission (section 3's `none` column is *not* this — that is `Granted`) |
| `NotDeclared` | the project config does not list it (section 7); a deployment bug, not a user one |
| `InvalidState` | a request for a permission that cannot be asked in its current state where the platform is ambiguous |
| `Platform` | the platform call itself failed; `detail` carries a stable, non-localised token |

`NotDeclared` is a distinct kind from `Unsupported` because they have different owners: one is a
missing manifest entry (the project's), the other a missing platform feature (the engine's).

## 7. Declaration in the project config

`tessera.json` gains one array, indexed in
[`project-config-v1.md`](project-config-v1.md)'s top-level table:

```json
"permissions": ["camera", "microphone", "notifications"],
"permissionText": {
  "camera": "To scan the code on the box.",
  "microphone": "To record your voice notes."
}
```

`permissionText` carries the usage description each permission needs on iOS and macOS, because those
OSes terminate a process whose bundle has an empty one. It is required for the permissions that need it
and accepted-and-ignored for the rest.

Rules, all enforced at build time:

1. **The ids are section 3's set.** An unknown id is a config error naming the valid set.
2. **The generator fails the build when code requests an undeclared permission.** The check is in
   the front end, so the error is a compiler diagnostic with a source span on the `request` call
   (`TESSERA_PERM_UNDECLARED`), not a runtime surprise on a device the developer may not have.
3. **Declaring a permission the app never requests is a warning, not an error.** It is a
   store-review smell rather than a correctness bug, and an app that ships a feature flag legitimately
   declares ahead of time.
4. **The declaration is the manifest's source.** The Android and iOS generators emit their entries
   (`<uses-permission>`, `NS*UsageDescription`) from this list, so the manifest cannot drift from the
   code. A permission needing a usage *string* (camera, microphone, location, photo library on iOS)
   gains an optional `permissionText` map, because those OSes refuse a bundle with an empty
   description.
5. **No permission is on by default.** An app that declares nothing can request nothing, and every
   request is `NotDeclared`. This is the least-privilege rule of `platform-modules.md` section 8
   made mechanical.

## 8. Platform mapping

The module normalizes; the back end does the platform thing. What each does:

**Android.** `requestPermissions` with `ActivityCompat` semantics; `shouldShowRequestPermissionRationale`
distinguishes `Denied` from `PermanentlyDenied` (false after a denial that the OS will not re-prompt).
`POST_NOTIFICATIONS` exists only on API 33+; below that the state is `Granted` because the permission
does not exist to be refused. `photo-library` maps to `READ_MEDIA_IMAGES` on 33+ and
`READ_EXTERNAL_STORAGE` below it.

**iOS.** `AVCaptureDevice.requestAccess`, `CLLocationManager` and `UNUserNotificationCenter`. The
`PermanentlyDenied` state is derived from `authorizationStatus` plus a completed request, because
`AVAuthorizationStatus.denied` alone does not tell you whether a prompt is still possible.

**Windows.** Camera, microphone and location go through the capability prompts of the packaged app
identity; an unpackaged app has no prompt and reports `Granted`. The rest are pickers (section 3).

**macOS.** Every usage string is mandatory; a missing one is a process termination by the OS, which
is why section 7 rule 4 makes the text a config input.

**Linux.** No prompt for anything; PipeWire (camera), the portal (files) and GeoClue (location) ask
on their own. The module reports `Granted` for the set and the compositor's dialogue is the user's
actual gate.

**A platform with no back end** (a headless build, the Wasm studio) reports `Unsupported` for every
request, and the studio panel (#448) scripts outcomes instead — see section 10.

## 9. Never

Stated as prohibitions because each has been a real bug in a real app:

1. **Never request at launch.** A prompt before the user has seen the feature is the behaviour
   section 1 describes; the module does not forbid it, but the generator warns on a `request` call
   reachable from a mount with no user gesture, and the guide says never.
2. **Never re-prompt a `PermanentlyDenied` permission.** Rule 3 of section 5 makes it impossible
   through this API, which is the point of routing every request through the module.
3. **Never infer a grant from a successful call.** A camera that opens is not proof the user granted
   the camera permission on platforms where a picker is involved.
4. **Never fabricate a prompt.** A desktop that has no permission model must not show an app-drawn
   dialogue pretending to be the OS one. `Granted` is the honest answer.
5. **Never cache a stale state across a resume.** The OS can change the answer while the app is
   backgrounded (the user visited Settings). `refresh()` re-reads on `Resume`; an app is never told
   a state the OS no longer holds.

## 10. Studio

The platform panel (#448) exposes each permission's state as an emulator control, so a design-time
preview can exercise all five states without a device. It drives the same in-memory fake the tests
use, which is the rule `studio-mock-services-v1.md` already states for the other services.

## 11. Open questions

1. **Should `request` take a group?** Android groups camera+microphone into one prompt on some
   versions; modelling that as one request would make the API match one platform's dialogue and lie
   about the others. Left per-permission in v1.
2. **What is the state of a permission the app has not declared but the OS allows?** The module
   reports `NotDeclared` from `request` and the platform's answer from `status`. That asymmetry is
   deliberate (one is the app's bug, the other a fact) but deserves review.
3. **Notification "provisional" authorization** (iOS `.provisional`) has no state in this model. It
   behaves as `Granted` with quiet delivery; a fourth-notification state is a v2 question.
4. **Should `file-access` be in the set at all**, given four of five platforms ask nothing? Kept
   because the user-visible question is uniform, but it is the weakest row in section 3.

## 12. What this spec does not change

- `ShellCapabilities::permissioned` stays. It is a per-operation mask for the *shell* (whether the
  clipboard or a dialog is usable right now), and a `PermissionService` result may feed it, but the
  two are not the same list and merging them would make the shell depend on a permission service it
  does not need.
- `media-components-v1.md`'s `permission_denied` stays. It is the recovery value a failed media load
  reports, and it now has a defined cause and a defined next step (this spec's `Denied` /
  `PermanentlyDenied` and section 4's table).
- No existing service gains a required dependency. A permission check is a call, not a link.
