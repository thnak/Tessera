## Background job contract and headless host (groundwork for #1043), notification fake delivery tests (#1036)

### What changed
- `BackgroundJob`, `JobConstraints`, `JobContext` (scope, clock, budget, checkpoint, one-shot `finish`), `JobScheduler` (the only OS seam): `include/tessera/appfw/background_job.hpp`.
- `FakeJobScheduler`: plays the OS (network/charging state, delay, `wake` with a reason).
- `HeadlessJobHost`: the headless `Application` entry. Launches services with no window, runs one job under budget minus margin, runs the checkpoint at budget or OS stop, closes the scope with TaskScope deadline rules, stops services in reverse order. Outcomes: Success, RetryRequested, Failed, BudgetExpired, Stopped, NotStarted.
- Notification contract suite: new `fake_delivery_paths` test (immediate vs scheduled, shown-id replace, platform refusal as a value, unknown-id fire, badge, cancel_all).
- Docs: `docs/design/background-work.md` section 6.1; public API baseline updated (additive, 15 lines).

### Verification
- Windows vs2026 Release: `appfw_background_job`, `appfw_notification` pass.
- `python -B scripts/gates.py --quiet`: 15/15. clang-format 22.1.3 applied to changed C++.
- Not run: Linux/GCC 15 build on thnak-server.

### What is left
- #1043 (stays open, narrowed): WorkManager and BGTaskScheduler `JobScheduler` back ends, the process entry mapping an OS wake to `begin(id, budget)`, the iOS expiration handler calling `request_stop()`, and starting a declared subset of services. Owner-gated on mobile back ends.
- #1036 (stays open, narrowed): the fake-delivery contract is covered; what remains needs an OS or a provider account (toast, UserNotifications, Android channels, D-Bus, FCM/APNs/WNS push ADR, launcher badges).

Refs #1043, Refs #1036
