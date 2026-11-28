# Slider: pointer drag (#982)

## What changed
- `packages/tessera-ui/primitives/Slider.tsx`: `onSizeChange` stores the track width; `onPointerDown` starts a drag and sets the value; `onPointerMove` while dragging maps node-local `e.x / width` to `min..max`, snapped to `step` and clamped; `onPointerUp` ends the drag. Capture is implicit, so out-of-box moves clamp. Width 0 (before first layout) changes nothing.
- `tests/unit/primitives_tests.cpp`: IR-evaluator tests for size store, press, snap, move, clamp both ends, move without press, zero width, release.
- Docs: component-library.md and library-farm-plan.md updated.

## Verification
- `primitives` ctest passes on Windows (vs2026 Release); `python -B scripts/gates.py --quiet` 15/15; clang-format 22.1.3 on the test file.
- No goldens: the primitive draws nothing.

## What is left
- `onPointerCancel` has no scene EventType (codegen rejects it), so cancel does not end a drag; release does.
- #305 (overlay non-modal focus needs an AppHost FocusManager hook) and #301 (SafeArea component defaults) were not started in this landing.

Refs #982, #532, #305, #301
Closes #982
