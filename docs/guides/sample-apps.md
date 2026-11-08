# Sample apps and templates

Status: planned (B2/C/F). Samples are executable specifications; each one must prove a slice of
the framework, not just look good.

## 1. Template set

| Sample | Proves |
|---|---|
| `hello` | window, scene, text, input, theme, headless/golden |
| `dashboard` | layout, responsive/fold, virtualization, charts, async data, audit |
| `notes` | text editing, forms, undo/redo, clipboard, overlays, state restore |
| `settings` | navigation, i18n, accessibility, platform shell, storage |
| `3d-viewer` | custom elements, shaders, 3D input/picking, headless GPU smoke |
| `chat` | lists, async data/offline, notifications, deep links, auth gate |

Each template ships:

- `tessera.json`, `ui/`, `src/main.cpp`, `elements.json` when relevant.
- tests: unit, structural headless, golden, audit profile.
- README with what it proves and what is intentionally out of scope.
- deterministic fixtures/assets/licensing.

## 2. Feature coverage

| Feature | hello | dashboard | notes | settings | 3d | chat |
|---|---|---|---|---|---|---|
| Window/DPI/resize | yes | yes | yes | yes | yes | yes |
| Layout/responsive | basic | yes | yes | yes | yes | yes |
| Text editing/IME | — | — | yes | — | — | yes |
| Forms/validation | — | filters | yes | yes | — | yes |
| Scroll/gestures | — | yes | yes | yes | yes | yes |
| Overlays/menus | — | yes | yes | yes | — | yes |
| Navigation/deep link | — | yes | yes | yes | yes | yes |
| State restore | — | yes | yes | yes | — | yes |
| i18n/RTL | — | yes | yes | yes | — | yes |
| Accessibility | yes | yes | yes | yes | fallback | yes |
| Custom elements/3D | — | charts | — | — | yes | — |
| Async data/offline | — | yes | — | — | — | yes |
| Updates/signing | packaging test | yes | yes | yes | yes | yes |

## 3. Commands

```bash
tessera new notes --template app
tessera build --preset dev
tessera test --script tests/agent/notes.json
tessera test --mode golden --out out/
tessera headless --mode audit --profile desktop --script tests/audit/dashboard.json
```

- Samples use the same public APIs as apps; no core shortcuts.
- Generated files are committed and deterministic.
- CI builds/tests samples on Windows/Linux; GPU smoke where available.
- Goldens use software/lavapipe, not real GPUs.
- Samples are versioned with the framework; breaking changes update templates + docs.

## 4. What samples must not do

- Depend on private headers or backend/OS handles.
- Hide business state in native globals.
- Use JS/React at runtime or dynamic plugins.
- Use real network/system fonts in deterministic tests.
- Ship secrets/PII in fixtures or screenshots.

## 5. Open questions

- Template generator vs checked-in samples.
- Maintenance cost and version skew.
- Which sample is the “golden path” for docs.
- Mobile-first sample after B3.
