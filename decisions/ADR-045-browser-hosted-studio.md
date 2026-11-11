# ADR-045: Browser-hosted designer studio and a stdio MCP adapter

Status: Proposed (2026-11-11).

## Context

[ADR-037](../../decisions/ADR-037-self-contained-wasm-designer.md) established a self-contained Wasm
engine (front end, IR evaluator, UI/rendering, WebGPU) with the browser page as a thin JavaScript
shell, and [designer-protocol-v1.md](../docs/specs/designer-protocol-v1.md) fixed the host/viewer
messages. That design still assumes a **native host**: an editor integration or the `tessera` CLI
owns the files and serves the page over a loopback WebSocket. `designer-protocol-v1.md` section 14
even records that a viewer without a host loads nothing.

Two product facts push past that assumption:

1. The cheapest first experience is a URL. A designer that needs a native toolchain (and a
   Vulkan-capable machine) before it shows anything is a design-time tool only the framework team can
   use. A static page with the Wasm engine can be opened by a designer, a reviewer, or a coding agent
   on any machine with a WebGPU browser.
2. A coding agent can consume the same engine headlessly. The repository already has the
   `tessera_harness` JSON-lines protocol for agent automation, but an agent still needs a native
   build to run it. A Node stdio entry of the same engine gives early validation (does this TSX
   compile? what does the scene look like?) without a toolchain.

Holding this back is also a UX question: the previous chrome work
([window-chrome-v1.md](../docs/specs/window-chrome-v1.md), ADR-044) says the OS owns the title bar and
system bars. A developer previewing a dark app must see honest chrome, not a white page frame, and
the studio must show it without turning the engine into a window manager.

## Decision

1. **The studio is a static site built from this repository and deployed to GitHub Pages**
   (`https://<org>.github.io/Tessera/designer/`). No server-side code, no build service, no upload
   endpoint. Deployments are CI artifacts, never committed binaries, and only `main` deploys.
2. **The page is the host.** It owns project files (memory, File System Access API, OPFS, zip),
   assigns file versions and applies patches; the viewer (Wasm in a worker) still never reads or
   writes files. The transport is `postMessage`, which the protocol already defines. The native CLI
   host remains for filesystem/codegen work.
3. **The engine renders only the preview canvas.** The studio's own chrome (toolbar, panels, editor)
   is HTML/CSS, never Tessera-rendered, so a preview failure cannot take down the editor and native
   text/file/a11y capabilities come for free.
4. **Simulated system chrome is a skin, not engine output.** Device frames and title/status/navigation
   bars are HTML/SVG around or over the canvas; the engine receives the production geometry (desktop
   client area with zero insets, mobile edge-to-edge with `safeAreas`). The skin is data plus static
   art; chrome colours come from the engine's resolved `{background, foreground, scheme}` record via
   a new `theme.applied` event, so preview chrome and production chrome cannot drift.
5. **Single-threaded Wasm on GitHub Pages.** No custom headers means no `SharedArrayBuffer` and no
   Wasm threads. Work off the main thread uses Web Workers and transferable buffers. If threads
   become necessary, the project either adopts a cross-origin-isolation service worker or moves the
   site to a header-capable static host; that is a follow-up decision, not a hidden dependency.
6. **MCP is a thin adapter, not a third protocol.** `tessera-mcp` runs the same engine in Node over
   stdio and maps each tool to a [headless-automation-v1.md](../docs/specs/headless-automation-v1.md)
   command or a designer-protocol message. A compile-only mode (`open_project`, `diagnostics`,
   `get_ir`) ships before the evaluator, so agents get value early.
7. **No vendor device art and no user code in the shell.** Bundled skins are original or
   permissively licensed; project files stay in the browser; user code is IR only, never JavaScript
   (ADR-024, ADR-036).

## Alternatives

- **Keep the native-host viewer only.** Rejected: requires a toolchain and a local server before the
  first pixel, which excludes designers, reviewers, agents and CI-only machines.
- **Use an Electron/Tauri desktop app for the designer.** Rejected: doubles the platform surface,
  adds a signed-binary distribution path, and solves none of the hosting problem.
- **Compile on a server (upload code, receive frames).** Rejected: project code would leave the
  machine, needs compute and abuse handling, and contradicts the local-only design.
- **Render the studio UI in Tessera itself.** Rejected for V1: bootstrapping a preview of the engine
  inside the engine makes a preview crash fatal to the editor and forces a second engine instance.
- **Require cross-origin isolation and Wasm threads from day one.** Rejected: GitHub Pages cannot
  set the headers; a service-worker shim is fragile and reload-heavy for a first release.
- **Expose the harness protocol directly to agents (no MCP).** Rejected as the primary path because
  agent tooling expects MCP; the harness stays the implementation and MCP the adapter.
- **Emulate a full OS (window manager, notifications, real status bar data).** Rejected: the studio
  is a UI preview, not a device emulator; the simulated chrome is geometry and theme only.

## Consequences

- The URL becomes the first contact with Tessera: size, startup time, browser support and CDN
  behaviour become product concerns, not afterthoughts.
- Two host implementations (native CLI and page) must satisfy one protocol; the studio cannot add
  private messages without a protocol change.
- A single-threaded engine may parse slower on large projects; the worker + transferable-buffer model
  and a component-granular parse keep it usable, but it needs measurement.
- GitHub Pages limits (one site per repo, no PR previews, 1 GB site cap, no headers) are accepted;
  a move to a header-capable host later is a deployment change, not an architecture change.
- Skins are a new art and data asset class with licensing rules; vendor device art is excluded.
- MCP gives agents an early path but also creates a second consumer of the engine's determinism and
  error contracts; the harness and designer specs stay the single source.
- `designer/web` and `rendering/backend/webgpu` are placeholders today; this ADR changes the target
  of Big Step D, not the amount of work already done.
