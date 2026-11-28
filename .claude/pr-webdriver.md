## Web app path: first emsdk compile, Node contract run, budgets (#969, partial)

### What changed
- samples/web_smoke: minimal web app (WebHost + RuntimeLoop, no renderer) built by the wasm-webgpu preset via tessera_web_app. First emsdk compile of web_exports.cpp, library_tessera_web.js and tessera_web_app: clean on emsdk 7.0.0 (-Werror).
- scripts/ci/web_smoke.mjs: runs the app under Node (linked web,node): one frame request per wake, input shares a pending frame, one C++ frame step consumes the event, parked app holds no callback.
- tools/web_budgets.json + same script: gzip budgets (wasm 17.3 KB / 48 KB, js 3.4 / 8 KB, host 2.6 / 4 KB) and a Node first-frame bound.
- .github/workflows/webgpu.yml: step in wasm-build; CMakePresets wasm-webgpu builds web_smoke; docs/design/web-target.md updated.

### Verification
Built and ran on thnak-server (emsdk 7.0.0, Node): all budgets ok, first frame 8 ms. gates.py --quiet 15/15, clang-format 22.1.3 clean.

### What is left
Web app driver for the gallery, real-browser run, gallery CI build/publish, text input/a11y/clipboard/files/persistence: https://github.com/thnak/Tessera/issues/1013.

Refs #969, #290
