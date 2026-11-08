# Exposing buffer/runtime tunables to users

Status: proposed (B2/B3). Companion to
[`buffer-config-v1.md`](../specs/buffer-config-v1.md) and
[`memory-model-v1.md`](../specs/memory-model-v1.md).

This guide is for developers who want a few safe parameters (buffer sizes, cache budgets,
list read-ahead, power/quality) to be user-adjustable.

## 1. Rule of thumb

Expose **capacity/budget/quality** knobs, not correctness/security/determinism internals.

Good candidates:

- upload/staging buffer size
- glyph atlas / texture cache budget
- layer/draw-list cache budget
- virtual list pool cap / overscan
- async queue capacity (restart)
- power/quality preset

Bad candidates:

- vertex format/alignment
- color space / golden mode
- `FLAG_SECURE` / security policy
- protocol/schema versions
- anything that changes pixels

## 2. Add a developer default

In `tessera.json`:

```json
{
  "buffers": {
    "renderer": { "stagingRingBytes": 16777216 },
    "ui": { "virtualListOverscanPx": 200 },
    "budgetBytes": 536870912
  }
}
```

The default is what the app uses when the user has not changed anything.

## 3. Expose a tunable

```json
{
  "tunables": [
    {
      "id": "renderer.stagingRingBytes",
      "type": "bytes",
      "labelKey": "settings.upload_buffer",
      "descriptionKey": "settings.upload_buffer.description",
      "min": 4194304,
      "max": 134217728,
      "step": 4194304,
      "scope": "restartRenderer",
      "category": "graphics",
      "powerImpact": "low"
    }
  ]
}
```

Checklist:

- [ ] id matches a real buffer/runtime value
- [ ] type/min/max/step are correct
- [ ] max <= engine hard cap
- [ ] scope is honest (`restartRenderer` for GPU buffers)
- [ ] label/description keys exist
- [ ] category/power impact are set
- [ ] the value cannot change pixels/security/determinism

## 4. Build the settings UI

Do not hardcode a row per tunable. Read metadata:

```cpp
for (const auto& t : tunables.list()) {
    // label = i18n(t.label_key)
    // widget by t.type, range t.min..t.max step t.step
    // show "restart required" if t.scope == RestartRenderer/RestartApp
}
```

On change:

```cpp
auto result = tunables.set(id, value);
if (!result) {
    show_error(result.error());
    return;
}
user_settings.save(id, value);
```

- `set()` clamps/aligns and applies per scope.
- Keep the old value if apply fails.
- Show the effective value after clamping.
- Provide "Reset" and "Reset all".

## 5. Apply scopes

| Scope | What the user sees |
|---|---|
| `live` | Applies smoothly at the next safe boundary |
| `nextFrame` | Applies next frame; may cause a one-frame layout/paint update |
| `restartRenderer` | Brief renderer reset; scene/app state preserved |
| `restartApp` | Prompt to relaunch |

For GPU buffers, prefer `restartRenderer`. Old resources retire after in-flight frames; do not
reallocate in the frame loop.

## 6. Persistence

- Store user overrides outside `tessera.json` (app data/OS preferences).
- Version the settings file; drop stale/unknown ids with a diagnostic.
- Do not persist non-exposed values.
- Provide reset-to-defaults and per-item reset.
- Never store secrets in tunables.

## 7. Budget and hard caps

- `buffers.budgetBytes` is a hard total; per-category budgets may also exist.
- If a user raises one buffer, reject or require lowering another; show why.
- Clamp to engine/platform hard caps; round to alignment.
- OOM: keep the previous generation, report an error, do not crash.

## 8. Testing

- Schema validation (min/max/step/type/id) and duplicate ids.
- Clamp/step/enum; budget overflow; hard-cap overflow.
- Apply scopes and failure rollback.
- Buffer resize: old generation retired after frames in flight; no validation errors.
- Persistence/reset/migration; stale ids dropped.
- Golden mode ignores user settings and stays pixel-identical.
- UI renders from metadata, not hardcoded rows.

## 9. Common mistakes

- Exposing every internal buffer.
- Min/max wider than the engine can handle.
- Letting users edit `tessera.json`.
- Reallocating a GPU buffer in the frame loop.
- Making a buffer size change pixels (should be capacity only).
- Forgetting restart/rollback messaging.
- Storing settings without a version/migration.

## 10. Status

- Spec/guide only; not implemented.
- Depends on `buffer-config-v1`, `memory-model-v1`, runtime config, and the settings service.
