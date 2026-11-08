# Loading strategy matrix

Status: proposed (Big Steps B2 and F; see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Companion to
[`startup-and-loading.md`](../design/startup-and-loading.md) (startup design and budgets) and
[`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md) (lazy/preload/lease/unload rules). Terms: first meaningful frame = first frame showing usable content; skeleton = placeholder layout shown before data arrives; lease/pin = a hold that prevents a resource from unloading.

Use this to choose startup, loading UI, auth, and resource policy. Default is **lazy +
critical-only eager + explicit preload + unload**.

## 1. Strategy trade-offs

| Strategy | Startup | First screen | Memory | Network/IO | Complexity | Best for |
|---|---|---|---|---|---|---|
| Eager everything | worst | fast after load | high | high | low | tiny apps, kiosk with fixed assets |
| Critical-only eager | good | good | low/medium | low | medium | most apps (default) |
| Pure lazy on demand | best | shell fast, content later | low | on demand | medium | content-first, large apps |
| Lazy + preload next | good | good | medium | predictive | medium/high | multi-screen apps, wizards |
| Streaming/progressive | good | fastest shell | low/medium | steady | high | media, charts, large data |
| Background prefetch | good | good | medium | background | high | dashboards, feeds |
| Cache + unload | good warm | good warm | bounded | cache | medium | long-running apps |

Rule: eager-load only what is required for the first meaningful frame. Everything else is
lazy/preload/unload.

## 2. App profile recommendations

| Profile | Startup | Loading UI | Auth | Resource policy |
|---|---|---|---|---|
| Kiosk / always-on dashboard | critical shell eager | shell + skeleton | none/device | lazy data, preload visible, unload offscreen, memory cap |
| Auth-first consumer | small splash, shell after auth | skeleton after auth | blocking gate | preload post-login home; lazy content; unload on logout |
| Content-first consumer | shell immediately | skeleton/progressive | deferred/guest | lazy content, preload on intent, cache+unload |
| Offline-first | cached shell | cache-first + stale badge | cached session | cache-first, background sync, bounded cache |
| Media-heavy | shell first | placeholders/skeletons | optional | lazy images/video, next-item preload, aggressive unload |
| Admin/data-heavy | auth first | table/chart skeleton | blocking | route-level lazy, virtualize lists, prefetch visible |
| Embedded/limited memory | minimal shell | simple progress | none | strict budgets, no background prefetch, aggressive unload |

## 3. Resource class defaults

| Resource class | Default | Preload | Unload |
|---|---|---|---|
| Bootstrap/config/theme tokens | eager (small) | — | never during app life |
| Critical font subset/icons | eager (small) | first screen | never while used |
| Route code/data | lazy | on intent/back-stack | route exit |
| Images | lazy | viewport proximity | route exit/memory pressure |
| Charts/media | lazy/streaming | next item/page | offscreen/timeout |
| Shader/pipeline | lazy at first use | first draw | device loss, or route exit if no longer used |
| User data/profile | after auth, lazy | post-auth home | logout |
| Diagnostics/tracing | lazy/off | debug only | release build off |

## 4. Decision checklist

- [ ] What is the first meaningful frame? Which resources are truly critical?
- [ ] Is auth blocking, overlaid, deferred, or background-refresh?
- [ ] What can render as shell/skeleton before data is ready?
- [ ] Which route/data/resources can be lazy?
- [ ] What preload triggers exist (intent, viewport, idle, next page)?
- [ ] What unload triggers exist (route exit, background, logout, memory pressure)?
- [ ] What are the startup/first-screen/route/memory budgets?
- [ ] What happens offline, on timeout, on auth failure, on OOM?
- [ ] Are loading/auth screens accessible and reduced-motion friendly?
- [ ] Are preloads cancellable and bounded?
- [ ] Are resources leased/pinned so they cannot unload while in use?
- [ ] Are metrics emitted for startup, route load, resource wait, and thrash?

## 5. Anti-patterns

- Loading every asset at startup "just in case".
- A blocking spinner for a screen that has a stable shell.
- Fake progress percentages.
- Double splash/loading screens.
- Auth screen that blocks the UI thread or flashes stale user data.
- Unbounded caches or preload queues.
- Unloading an active/pinned resource.
- Reloading the same resource every frame (thrash).
- Starting preloads that are never cancelled.
- Minimum loading-screen time longer than the actual load.

## 6. Example: auth-first dashboard

```text
startup:
  critical: config, theme, font subset, auth module
  shell: not shown until auth check
auth:
  restore token async; refresh if needed; show auth screen on failure
post-auth:
  show shell + skeleton
  preload: home route shell, visible charts, user profile
  lazy: images, secondary routes, reports
  unload: on logout, route exit, background (per the app's policy), memory pressure
```

Budgets (desktop dashboard targets from [startup-and-loading.md](../design/startup-and-loading.md), which also gives < 1 s cold): first frame < 300 ms warm; auth cached check < 150 ms; route shell < 100 ms; data streams;
no full-screen spinner after the shell exists.

## 7. Example: kiosk dashboard

```text
startup:
  critical: shell, theme, font subset, visible widgets
  no auth (or device auth)
loading:
  shell immediately; skeleton for data
resources:
  lazy offscreen widgets; preload next rotation; unload offscreen images
  strict memory cap; no background network prefetch on battery
```

## 8. Status

- Matrix/guide only; not implemented.
- Mobile budgets: not specified (no numbers in any document). [`startup-and-loading.md`](../design/startup-and-loading.md) states only qualitatively that they are stricter than desktop. Measure on real devices.
- Record the chosen profile in the app's design doc and test it in CI (no CI job for this exists yet).
