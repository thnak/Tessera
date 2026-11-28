## appfw: notifications spec and NotificationService (#452, partial)

### What changed
- `docs/specs/notifications-v1.md`: request model, consent, activation (cold and hot), platform mapping table, seam and fake, what needs OS delivery. `os-events-v1.md` section 2.4 names the two events (no second event path).
- `tessera_appfw_notification`: `NotificationBackend` seam, `FakeNotificationBackend`, `NotificationService` (consent via `PermissionService`, channels, dedup id, pending bound, revocation sync, badge, activation hold/deliver), `route_notification_activation` over `install_deep_link` with `ActivationSource::Notification`.
- Public API baseline updated (+16 additive), docs/README folded to stay at 600.

### Verification
- ctest `appfw_notification` (Windows vs2026 Release): schedule then cancel, dedup, channel/bound/capability, denied/permanently denied/undeclared as values, revocation, cold and hot tap to deep link, hostile and empty links.
- `python -B scripts/gates.py --quiet`: 15/15; clang-format 22.1.3 clean.

### What is left
Real back ends and push (FCM, APNs, WNS) need an OS or provider account: https://github.com/thnak/Tessera/issues/1036. No ADR written: nothing contentious until push providers are chosen.

Refs #452
