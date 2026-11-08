# Buffer configuration and user tunables v1

Status: proposed (Big Step B2/B3). Normative companion to
[`memory-model-v1.md`](memory-model-v1.md), [`runtime-config-v1.md`](runtime-config-v1.md),
and [`project-config-v1.md`](project-config-v1.md).

This spec covers engine buffer capacities (CPU/GPU/queues/caches) and the **explicit allowlist**
of parameters a developer may expose to end users.

## 1. Principles

1. **Developer sets defaults; users may tune only exposed values.** `tessera.json` is not a
   user-editable file.
2. **No unbounded values.** Every tunable has a type, min, max, step, and apply scope.
3. **Hard caps win.** User values are clamped to engine/platform hard caps; invalid values are
   rejected with a clear error.
4. **Memory budget wins.** The sum of buffer budgets cannot exceed the configured memory budget;
   raising one buffer may require lowering another.
5. **Buffer sizes must not change pixels.** They change capacity/performance, not visual output.
   Golden mode ignores user tunables and uses fixed defaults.
6. **No hot-path reallocation.** Resizes create a new generation; old buffers retire after
   in-flight frames complete (ADR-020/021).
7. **Native-only.** Tunables are applied through a C++ registry; no production bridge/JS.
8. **Few knobs, good defaults.** Expose presets or a small curated set, not every internal
   buffer.

## 2. Buffer categories

| ID / area | What | Default owner | User-tunable? |
|---|---|---|---|
| `renderer.frameArenaBytes` | per-frame scratch/command memory | renderer | no (advanced) |
| `renderer.vertexInitialVertices` | initial vertex buffer capacity | batched2D | no |
| `renderer.vertexMaxVertices` | hard vertex capacity per frame | batched2D | maybe (advanced) |
| `renderer.stagingRingBytes` | upload staging ring | resources | yes |
| `renderer.readbackBytes` | readback buffer | renderer/diagnostics | no |
| `renderer.glyphAtlasBytes` | glyph atlas budget | text | yes |
| `renderer.textureCacheBytes` | texture cache budget | resources | yes |
| `runtime.layerCacheBudgetMb` | retained layer cache | runtime | yes |
| `runtime.drawListCacheBudgetMb` | draw-list cache | runtime | yes |
| `runtime.asyncQueueCapacity` | async task/completion queue | runtime | yes (restart) |
| `input.queueCapacity` | normalized event queue | input | no |
| `ui.virtualListPoolCap` | recycled item node pool | virtualization | yes |
| `ui.virtualListOverscanPx` | list/grid read-ahead | virtualization | yes (live) |
| `services.networkBufferBytes` | service I/O buffers | services | maybe |
| `services.storageBufferBytes` | service I/O buffers | services | maybe |

Existing runtime fields (`layerCacheBudgetMb`, `drawListCacheBudgetMb`,
`asyncQueueCapacity`, `gpuFramesInFlight`) remain in `runtime`; do not duplicate them under
`buffers`. One source of truth per value.

## 3. Developer configuration

`tessera.json` gains two optional sections:

```json
{
  "buffers": {
    "renderer": {
      "frameArenaBytes": 8388608,
      "vertexInitialVertices": 4096,
      "vertexMaxVertices": 262144,
      "stagingRingBytes": 16777216,
      "glyphAtlasBytes": 33554432,
      "textureCacheBytes": 134217728
    },
    "input": { "queueCapacity": 2048 },
    "ui": { "virtualListPoolCap": 256, "virtualListOverscanPx": 200 },
    "budgetBytes": 536870912
  },
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
    },
    {
      "id": "ui.virtualListOverscanPx",
      "type": "pixels",
      "labelKey": "settings.list_read_ahead",
      "min": 0,
      "max": 2000,
      "step": 50,
      "scope": "live",
      "category": "ui",
      "powerImpact": "medium"
    }
  ]
}
```

Rules:

- `buffers` keys are developer defaults; nested paths map to tunable ids.
- `tunables` is an explicit allowlist. A value not listed is **not** user-tunable.
- `type`: `bytes`, `count`, `pixels`, `float`, `bool`, `enum`.
- `scope`: `live`, `nextFrame`, `restartRenderer`, `restartApp`.
- `min`/`max`/`step` are required for numeric types; `enum` requires `values`.
- `labelKey`/`descriptionKey` are localization keys, not raw user strings.
- `category` groups settings UI (`graphics`, `ui`, `power`, `diagnostics`, ...).
- `powerImpact`: `none`, `low`, `medium`, `high` (advisory for UI).
- `budgetBytes` is the app's total buffer budget; per-category budgets may also be added.

## 4. Precedence

Highest to lowest:

1. **Deterministic/golden mode**: fixed defaults; user/CLI overrides ignored.
2. **CLI developer override** (one run; only allowed debug flags).
3. **User settings** (only exposed tunable ids).
4. **Project `buffers`** (developer defaults).
5. **Engine defaults**.

User settings never rewrite `tessera.json`; they live in app/user storage (platform settings or
`user-settings.json` in the app data directory). The file is versioned and migratable.

## 5. Validation and safety

- Unknown tunable id -> error (developer config) or ignored-with-diagnostic (stale user setting).
- `min <= default <= max`; `step > 0`; `max` must not exceed the engine hard cap.
- User values are clamped to `[min, max]` and aligned to `step`; invalid enum -> reject.
- Sum of buffer budgets must be <= `budgetBytes`; otherwise reject the config or clamp with a
  warning (choose one policy and test it).
- Buffer sizes must be aligned to the backend requirement (e.g., 4/16/256 bytes); the engine
  rounds up internally and reports it.
- A user value may change performance/power, never correctness or pixels.
- Security/determinism-critical values are **not** tunable (e.g., vertex format, alignment,
  color space, `FLAG_SECURE`, golden mode).
- Queue capacities are bounded; overflow applies backpressure/cancellation, never OOM.
- OOM at a larger size: keep the previous generation, report a diagnostic, do not crash.

## 6. Apply scopes

| Scope | Behavior |
|---|---|
| `live` | Update at the next safe boundary; no visible stall; may allocate new storage and retire old after frames |
| `nextFrame` | Apply at the next frame/tick; may cause one layout/paint invalidation |
| `restartRenderer` | Recreate buffers/targets/pipelines; keep app/scene state; no app restart |
| `restartApp` | Persist and require relaunch; show a clear prompt |

Rules:

- GPU buffer resize creates a new generation; old buffers stay until in-flight frames retire
  (ADR-020).
- No realloc in the frame hot path; do it at the boundary.
- If apply fails, keep the old value and report an error; do not leave a half-applied state.
- `restartApp` values are stored as pending until the app restarts.

## 7. C++ API

```cpp
enum class TunableType { Bytes, Count, Pixels, Float, Bool, Enum };
enum class TunableScope { Live, NextFrame, RestartRenderer, RestartApp };

struct TunableDesc {
    std::string id;
    TunableType type;
    TunableScope scope;
    std::string label_key;
    std::string description_key;
    std::string category;
    double min = 0;
    double max = 0;
    double step = 0;
    std::vector<std::string> enum_values;
};

class TunableRegistry {
public:
    Status register_tunable(TunableDesc desc);
    Status set(std::string_view id, TunableValue value);
    TunableValue get(std::string_view id) const;
    std::vector<TunableDesc> list() const;
    Status reset(std::string_view id);
    Status reset_all();
    std::uint64_t memory_budget_bytes() const;
};
```

- Registration happens at startup from validated config.
- `set()` validates/clamps and returns an error for unknown/non-exposed ids.
- `list()` drives the settings UI; no hardcoded UI per tunable.
- `reset` restores the developer default.
- The registry is UI-thread-owned; no bridge.

## 8. User settings

- Stored outside the project (app data/OS preferences), keyed by tunable id.
- Versioned (`tunableSettingsVersion`); unknown ids are dropped with a diagnostic.
- Only exposed ids are loaded; non-exposed values are ignored.
- Reset-to-defaults and per-item reset are supported.
- Settings survive app updates unless the tunable is removed/renamed (migration note).
- Sensitive values are never stored in user settings.

## 9. UX guidance

- Show label, current value/unit, min/max, step, restart requirement, and power impact.
- Prefer presets (Low/Medium/High) over dozens of raw numbers.
- Show an estimate of memory use and a warning near the budget.
- Disable apply when invalid; explain why.
- Do not expose diagnostics/security/correctness internals.
- Keep the basic set small; put advanced buffers behind an "Advanced" section.

## 10. Tests

- Schema: duplicate ids, missing label, invalid min/max/step, non-exposed user setting.
- Clamp/step/enum validation; budget overflow; hard-cap overflow.
- Apply scopes: live/nextFrame/restartRenderer/restartApp; failure keeps old value.
- Buffer resize: new generation, old retired after frames in flight, no validation errors.
- Persistence: save/load/reset/migrate; stale ids dropped.
- Golden: user tunables ignored; output pixels unchanged by buffer-size changes.
- OOM: fallback to previous size, diagnostic, no crash.
- UI: metadata drives settings; no hardcoded per-tunable UI.

## 11. Open questions

- Which buffers are safe to expose by default (staging/glyph/texture/list pool)?
- Presets vs raw numeric controls.
- Live GPU buffer resize vs `restartRenderer` only.
- Per-platform hard caps and budget defaults.
- Whether per-category budgets are user-visible.
- Where to persist user settings on Windows/Linux/Android/iOS.
- How to present power impact without a reliable platform metric.
