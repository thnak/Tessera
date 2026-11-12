# Runtime configuration examples

Worked `runtime` blocks for the common targets, with the resulting frame-cap behaviour. The
normative rules these examples satisfy — field ranges, validation, the divisor rule and the
wiring status — are in [`runtime-config-v1.md`](../specs/runtime-config-v1.md); this page does
not restate them.

## 144 Hz VRR dashboard

```json
{
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 0,
    "vsync": "adaptive",
    "dynamicRefreshRate": "auto",
    "framePacing": "smooth",
    "latencyFrames": 2,
    "dirtyRendering": "auto",
    "damageRects": true,
    "renderOnDemand": false,
    "powerPolicy": "adaptive"
  }
}
```

Content presents when ready; VRR follows the content rate up to the display maximum. The
simulation still ticks at 60 Hz. `renderOnDemand=false` is explicit here because the dashboard is
continuous; omit it to use the adaptive/idle default.

## 60 Hz low-latency tool UI

```json
{
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 60,
    "vsync": "mailbox",
    "dynamicRefreshRate": "off",
    "framePacing": "low-latency",
    "latencyFrames": 1,
    "renderOnDemand": true,
    "dirtyRendering": "on",
    "damageRects": true,
    "powerPolicy": "adaptive",
    "adaptiveQuality": true
  }
}
```

Static UI renders only on input/dirty wake; pointer-driven feedback has the lowest queue depth.

## 120 Hz cap on a 144 Hz display

```json
{ "runtime": { "maxFps": 120, "vsync": "on", "dynamicRefreshRate": "off" } }
```

The pacer uses a deadline schedule; it does not busy-wait. If a refresh divisor is required for
stability, use `maxFps=72` on a 144 Hz display instead.

## Power-save kiosk

```json
{
  "runtime": {
    "powerPolicy": "power-save",
    "maxFps": 30,
    "renderOnDemand": true,
    "pauseWhenOccluded": true,
    "adaptiveQuality": true
  }
}
```

The explicit `maxFps` overrides the policy preset. Static pages idle; animations/input are
capped at 30 fps and adaptive quality protects the budget.

## Benchmark mode (diagnostics only)

```json
{
  "runtime": {
    "powerPolicy": "benchmark",
    "maxFps": 0,
    "dirtyRendering": "off",
    "damageRects": false,
    "adaptiveQuality": false
  }
}
```

This is a profiling mode. It must not ship; tooling rejects it for release packaging.

## Frame-cap outcomes

| Display | `maxFps` | `vsync` | Effective |
|---|---|---|---|
| 60 Hz | `0` | `on` | 60 |
| 144 Hz | `0` | `on` | 144 |
| 144 Hz | `72` | `on` | 72 (divisor) |
| 144 Hz | `120` | `on` | 120 (deadline) |
| 144 Hz VRR | `0` | `adaptive` | content rate, up to 144 |
| any | `0` | `off` + tearing | uncapped, may tear |
| static UI | `30` | any | 30 while active, idle when on-demand |

## Related

- [`runtime-config-v1.md`](../specs/runtime-config-v1.md) — the schema and the rules.
- [`game-loop.md`](../design/game-loop.md) — pacing, refresh and interpolation.
- [`performance.md`](../design/performance.md) — the adaptive-quality decision order.
