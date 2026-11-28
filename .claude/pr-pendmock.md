## Resources panel in the browser: hand ui/services.json to the engine, browser gate (#976, partial)

### What changed
- Found while writing the gate: the page never passed the project's `ui/services.json` to the engine (`ProjectSnapshot` had no manifest and the file classified as locked `other`), so in the real Studio a `useResource` component had no service to bind and the Resources panel could not appear. Now: `FILE_KIND.services` (text, editable), `projectSnapshot` carries `servicesManifest`, and `stageProject` already forwarded it.
- `scripts/ci/preview_browser_resources.mjs`: Playwright gate for the Resources panel (opens on fallback, Resolve, stale-epoch drop, Fetch holds content with isPending, Fail keeps data, malformed epoch refused). `studio_project.mjs` `ensureProject` takes `extraFiles`/`force`.
- Node gate additions: snapshot carries the manifest (`preview_resource_controls.mjs`), `classifyPath` for `ui/services.json` (`preview_project_paths.mjs`).
- Spec: pending-ui-v1 section 10.4.

### Verification
- `node scripts/ci/preview_resource_controls.mjs` and `preview_project_paths.mjs` pass; `tsc --noEmit` clean; `python -B scripts/gates.py --quiet` 15/15.
- The browser gate is syntax-checked only: it needs the built Pages site (emsdk Wasm modules) and was not run here.

### What is left
- Mock-worker resource script hook: owner decision (ADR-055 covers service-call responses only).
- Browser gate run against a built site, and CI wiring in pages.yml once it has been seen green.
- Resource report carrying a current value; Suspense boundaries inside a callee instance.

Refs #976
