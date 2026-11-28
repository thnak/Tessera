# Notifications v1

Status: draft, normative once accepted (issue #452). Until now notifications existed as one rule
in [`platform-modules.md`](../design/services/platform-modules.md) section 6 ("no notification
without user consent") and a row in [`startup-api-v1.md`](startup-api-v1.md) section 5.1. This
spec owns the model; [`permissions-v1.md`](permissions-v1.md) owns consent and
[`deep-linking-v1.md`](deep-linking-v1.md) owns what a tap opens.

It fixes: the **request model** (section 3), **consent** (section 4), **activation** (section 5),
the **platform mapping** (section 7) and the **seam and fake** (section 8). It also says plainly
what needs an OS and is not built (section 9).

## 1. Rules

1. Nothing is posted without a granted `notifications` permission. Refusal is a value.
2. The service never prompts. Asking is the app's call, through the permission service.
3. A tap never reaches the UI as an OS intent: it is one normalized activation, routed through the
   deep-link path, so auth gates and allowlists apply unchanged.
4. A notification link is untrusted input, validated when it is activated, not when scheduled.
5. Deterministic: no wall clock is read; delivery time is a number on the app's injected clock.

## 2. Terms

| Term | Meaning |
|---|---|
| **channel** | a user-silenceable category the app declares up front (Android channel, iOS category) |
| **dedup id** | the app's id for one notification; reuse replaces |
| **armed** | scheduled for a future time, not yet shown |
| **activation** | a tap on a notification body or one of its actions, normalized |

## 3. Request model

```cpp
struct NotificationRequest {
    std::string id;                 // dedup id, required
    std::string channel;            // must be configured
    std::string title, body;        // at least one non-empty
    std::uint64_t deliver_at_ms;    // 0 = now
    std::vector<NotificationAction> actions;
    std::string link;               // deep link opened by a tap
};
```

Outcomes are `NotificationStatus` values: `Scheduled`, `Replaced`, `PermissionNotGranted`,
`InvalidRequest`, `UnknownChannel`, `TooManyPending`, `Unsupported`.

1. Channels come from `NotificationConfig` and are checked on every request; an unknown channel is
   `UnknownChannel`, never a silent default.
2. The same id replaces the earlier notification, armed or shown (`Replaced`).
3. Armed notifications are bounded (`max_pending`, default 64); an immediate one is not tracked.
4. `cancel(id)`, `cancel_all()` and `on_delivered(id)` maintain the armed set; the platform adapter
   calls `on_delivered` when an armed notification is shown.
5. Badge: `set_badge(count)` needs consent too and is `Unsupported` where the back end has none.

## 4. Consent

1. `schedule` and `set_badge` read the cached `PermissionService::status(Notifications)`. Not
   `Granted` (or not declared in the project config) is `PermissionNotGranted`, and the result
   carries the permission state so an app can offer Settings for `PermanentlyDenied`.
2. Consent is checked before arguments, so a denied app learns nothing about them and the back end
   is never called.
3. Revocation: `sync_consent()` (called after a permission `refresh()`, i.e. on `Resume`) cancels
   every armed notification once the permission is no longer granted, and returns how many.
4. A desktop platform with no permission model reports `Granted` (permissions-v1.md section 3), so
   the same code path posts there.

## 5. Activation

1. The platform adapter reports a tap with `on_activation`. The service sets `cold_start`, not the
   platform: no handler installed means the app was launched by the tap.
2. **Cold start.** The activation is held (bounded by `max_held_activations`, default 4, oldest
   dropped) and delivered with `cold_start = true` when the app installs its handler, which is the
   "wait for readiness; preserve the pending link" rule of navigation-v1.md section 5.
3. **Hot resume.** With a handler installed the activation is delivered immediately,
   `cold_start = false`.
4. `route_notification_activation` parses the link with `ActivationSource::Notification` through
   `install_deep_link`: a cold tap synthesizes a stack below the target, a hot tap pushes. An empty
   link is `NotFound` (open the app and stop); a link outside the scheme/host allowlists is refused.
5. Action buttons carry `action_id`; an action with sensitive parameters follows deep-linking-v1.md
   section 8 and is confirmed by the host.

## 6. Normalized events

Delivered through the one OS-event path, not a second one
([`os-events-v1.md`](os-events-v1.md) section 2.4): `NotificationActivated` (the activation above)
and `NotificationDelivered` (id). Push token and push payload events are reserved there and not
implemented (section 9).

## 7. Platform mapping

| Concern | Windows | macOS / iOS | Android | Linux desktop |
|---|---|---|---|---|
| Post | toast (`ToastNotificationManager`) | `UNUserNotificationCenter` | `NotificationManagerCompat` | `org.freedesktop.Notifications` D-Bus |
| Schedule | scheduled toast | `UNTimeIntervalNotificationTrigger` | `AlarmManager` + post | no-op: `Unsupported` (app timer) |
| Channel | no-op (tag) | category / thread id | `NotificationChannel` (required, API 26+) | no-op (category hint) |
| Actions | toast buttons | `UNNotificationAction` | `addAction` | server capability, else no-op |
| Badge | taskbar overlay | app badge | launcher-dependent, else `Unsupported` | `Unsupported` |
| Consent | none: `Granted` | request | `POST_NOTIFICATIONS` (13+) | none: `Granted` |
| Tap, cold | activation argument | launch options | launch intent | activation action / argv |
| Tap, hot | activation callback | delegate callback | `onNewIntent` | D-Bus `ActionInvoked` |

## 8. Seam and fake

`NotificationBackend` (`include/tessera/appfw/notification.hpp`) is the whole platform boundary:
`post`, `cancel`, `cancel_all`, `set_badge` and `capabilities`. `FakeNotificationBackend` is the
in-tree back end (armed list, shown list, `fire(id)`, badge, call counter, refuse switch).
`NotificationService` (`notification_service.hpp`) owns sections 3 to 5 once. The module is
`tessera_appfw_notification`, over the permission and navigation modules.

## 9. What needs OS delivery and is not built

- **Per-platform back ends** (section 7): each needs its OS and a device or CI lane.
- **Push** (FCM, APNs, WNS): token registration and remote payloads need provider accounts, a
  server and credentials; the token/payload events are reserved, the service has no push surface.
  Tracked in a follow-up issue.
- iOS provisional authorization (permissions-v1.md section 11 item 3) and notification history
  (live-capsule-v1.md non-goal).

## 10. Tests

`tests/unit/appfw_notification_tests.cpp` (ctest `appfw_notification`): schedule then cancel and
delivery; deduplication, channel, bound and capability rules; denied, permanently denied and
undeclared permission return a value and never call the back end; revocation; a cold tap held then
routed to a synthesized stack; a hot tap pushed onto the live stack; hostile and empty links; the
held-activation bound. All fakes, no clock, no OS.
