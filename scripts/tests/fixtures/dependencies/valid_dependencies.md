# Dependency map (fixture)

Synthetic table used by `scripts/tests/test_check_dependencies.py`. It exercises the gate rules
without touching the real dependency map.

## 1. Runtime

| Library | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Alpha | V1 hard | MIT | `core` | fixture core dependency |
| Beta | V1 optional | BSD-3-Clause / IJG | `ui/image` | fixture image codec |
| Gamma | V1 hard (vendored v1.2.3) | FreeType License (BSD-style) | `ui/text` | fixture font rasterizer |
| Delta | V1 transitive | zlib | `rendering/resources` | fixture compression |
| Epsilon | V1 optional | Public domain / MIT | `hal/audio` | fixture audio |
| Zeta | V1 hard | Unicode License | `ui/text` | fixture unicode |
| Eta | V1 optional | ISC | `services/networking` | fixture crypto |
| Theta | V1 optional | curl (MIT-like) | `services/networking` | fixture http |
| Iota | V1 optional alternative | BSL-1.0 | `core` | fixture formatting |
| Kappa | V1 optional | OFL-1.1 | `assets/fonts` | fixture bundled font |
| Lambda | V1 test | MIT | `tests/unit` | fixture test helper |
| Tau | V1 target (owner-approved) | MIT | `rendering/backend` | approved V1 target dependency |
| Upsilon | Fallback only | BSD-3-Clause | `rendering/backend` | not selected; no notice required |
| Mu | V1 optional build | GPL-3.0-or-later | build | compiler cache; build-time only, never linked |
| Nu | Deferred | System (AT-SPI: LGPL) | `ui/accessibility` | system accessibility integration |

## 2. Platform and found

| Library / API | Status | License | Module | Role in Tessera |
|---|---|---|---|---|
| Rho | System | System | `platform/windows` | system window API |
| Rho | System | System | `platform/windows` | duplicate row (same status, warning only) |
| C++23 standard library | V1 hard | — | all | standard library, not a third-party component |
