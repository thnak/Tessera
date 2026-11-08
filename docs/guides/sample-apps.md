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

## 4.1 Detail design

CI/maintenance:

- Each sample has a pinned profile (size/DPI/locale/theme), deterministic assets, and a
  `tests/` script. CI builds/tests on Windows/Linux; GPU smoke is optional/labeled.
- Generated files are checked by `tessera generate --check`; no stale templates.
- Samples use only public headers/APIs; a private-header dependency is a build failure.
- Golden baselines are owned by the framework repo and reviewed with the sample change.
- A sample may skip a feature only if the README states why and the coverage matrix marks it.

Template contract:

- `tessera.json`, `ui/`, `src/main.cpp`, `tests/`, `README.md`, and (if needed) `ui/elements.json`.
- No secrets/real network in default tests; fakes/fixtures are local and deterministic.
- The `notes` sample is the golden path for text/forms/restore; `3d-viewer` for custom elements;
  `settings` for navigation/i18n/a11y/shell; `dashboard` for layout/data/audit; `chat` for
  offline/deep links.

Maintenance risk:

- Samples are versioned with the framework; breaking changes update the template and docs in
  the same change.
- A sample that cannot be maintained is moved to `examples/archive` or removed, not silently
  broken.
- Mobile sample is deferred until the Android backend exists.

Gap-closure decisions:

- **G-26**: samples are checked-in and generator-compatible (the generator emits the same
  templates); CI builds/tests them on Windows/Linux, `generate --check` rejects drift, and
  goldens are owned/reviewed by the framework. `notes` is the docs golden path. A mobile sample
  is deferred until the Android backend exists.

## 5. Deferred/open questions

- Maintenance cost/version skew after 1.0.
- Mobile-first sample (post-B3).
- Whether archived examples stay in-tree or move to a separate repo.
