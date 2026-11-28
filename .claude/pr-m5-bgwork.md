## Background work policy (#453)

- Decision (docs/design/background-work.md sections 5-6): V1 background work is the in-process TaskScope; OS-scheduled jobs are deferred and owner-gated, with the intended BackgroundJob / headless-entry shape and platform map recorded. No ADR: status quo of ADR-050 section 5.
- New appfw::BackgroundCoordinator (BackgroundPolicy Continue/Throttle/Pause, checkpoint on entering the background, cancel non-Continue scopes and pause timers on Suspended).
- TimerQueue::set_paused (layer-0 seam): paused queue fires nothing and reports kNoDeadline; overdue timers fire after resume, periodic coalesces.
- Tests: tests/unit/appfw_background_tests.cpp (ManualClock, hand-drained CompletionQueue, explicit timer instants).

## Verification
- MSVC release: appfw_background, appfw_task_scope, runtime_timer pass. gates.py --quiet 15/15 (public-api baseline updated, additive). clang-format 22.1.3 applied.
- Linux/GCC build not run.

## Left
- Wiring the coordinator into Application's root scope (waits for the app root scope, see design section 4).
- OS-scheduled jobs: #1043.

Refs #453
Closes #453
