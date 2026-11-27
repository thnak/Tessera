// SPDX-License-Identifier: MIT

// The Canvas view's session (P3, issue #167; the React host is issue #422).
//
// **This is where ADR-054 decision 3 is load-bearing.** The engine draws exactly one thing -- the preview
// canvas -- and the page remains its host. React renders the `<canvas>` element *once*, as a childless leaf
// with a ref, and never again: no changing props, no children, no key, so React never replaces or reconciles
// it. Everything after that is this session's: the backing-store size, the WebGPU context, the
// `requestAnimationFrame` that draws, and the failure states. A preview crash therefore loses the canvas,
// not the editor; and React's render cycle never runs while a GPU frame is being recorded.
//
// Four things this module is careful about, because each is a requirement rather than a detail:
//
//  1. **Render on demand.** A static scene draws once. A frame is requested only when something changed: an
//     applied edit that the module answered `changed:true`, a device-pixel-ratio change, a toolbar change, or
//     the pane becoming visible again. There is no `requestAnimationFrame` loop spinning at 60 Hz -- the
//     repository's idle rule (a static UI must present nothing) applies to the browser page too. The frame
//     callback is gated on the view being visible, so a hidden pane never draws.
//
//  2. **The unsupported path is a state, not a blank canvas.** `canvas-device.ts` turns every way of
//     failing -- no `navigator.gpu`, no adapter, `requestDevice()` rejecting, a module without the handover
//     export -- into a sentence the pane shows with a pointer at the Scene tab.
//
//  3. **The device handover is explicit and happens once.** A device is acquired on the first *apply* and
//     reused; re-running the handover per apply would leak a device per keystroke. A *lost* device is the one
//     case that re-acquires, and it says so.
//
//  4. **The module and the device are lazy.** The module is imported when the Canvas view is first opened, and
//     the device is acquired by the first apply there -- which, since #444, is the first automatic one rather
//     than a button press. The unsupported state is decided from `navigator.gpu`'s presence, which needs no
//     async work and no download, and is shown the moment the tab opens -- so a browser without WebGPU never
//     downloads `tessera_canvas.wasm`, and a visitor who never opens Canvas takes no GPU.
//
//  5. **Applying an edit is not a restart (issue #444).** `apply()` re-evaluates the running session and lets
//     the engine carry state by its own rule; a source that does not evaluate leaves the evaluation and the
//     picture alone. Only `run()` -- the **Restart app** button -- releases the session, which is what resets
//     every cell to its initializer. Nothing on the edit path may call `pane.hideStates()`, `showFailed` or
//     any other state transition: those are the ways a canvas goes blank, and a blank canvas mid-word is the
//     failure this slice exists to prevent.
//
// What the view does NOT claim: the pixels are the module's, but the comparison against the lavapipe golden
// is a CI/browser-lane job (`scripts/ci/scene_frame_parity.py`). This module cannot read pixels back
// synchronously, which is why no report it returns claims `pixelsAvailable`.

import { wasmModuleUrl } from "../lib/module-url";
import type { ProjectReader, StatusReporter, ViewSession } from "../lib/views";
import { callExport, requireExports, stageProject, type WasmModule } from "../lib/wasm-call";
import { acquireDevice, handOverDevice, type CanvasModule } from "./canvas-device";
import { loadFontFor } from "./canvas-font";
import {
  requestedViewport,
  sizeToDevicePixels,
  type CanvasApplyReport,
  type CanvasFrameReport,
  type CanvasStateCell,
  type CanvasViewportReport,
} from "./canvas-frame";
import { attachCanvasKeyboard } from "./canvas-keyboard";
import { sendPlatformEvent } from "./canvas-platform";
import { unavailableReply } from "../lib/platform-events";
import { attachCanvasPointer } from "./canvas-pointer";
import {
  createCanvasInspector,
  type CanvasInspector,
  type CanvasStyleReport,
} from "./canvas-inspector";
import { createCanvasPane, type CanvasPane, type CanvasPaneElements } from "./canvas-pane";
import { createCanvasResize } from "./canvas-resize-layer";
import { cssZoomOf } from "./canvas-zoom";
import { createCanvasPicker, type CanvasPicker } from "./canvas-picker";
import type { CommitBridge } from "../lib/override-client";
import { createCanvasOverrides } from "./canvas-overrides";
import type { OverrideDomainHost } from "../lib/edit-routing";

export type { CanvasApplyReport, CanvasFrameReport, CanvasStateCell };

/**
 * The skin's safe area as the canvas module takes it (issue #171's skins slice): the insets a device
 * would report as `WindowMetrics::insets` and its cutout rectangle, in viewer coordinates.
 *
 * `null` from the reader means "no skin", which is the pre-#171 page: the session then sends zeros,
 * so a visitor who never picks a skin lays out exactly as before.
 */
export interface SkinSafeArea {
  insets: { top: number; right: number; bottom: number; left: number };
  /** The cutout rectangle, or `null` for none. */
  cutout: { x: number; y: number; width: number; height: number } | null;
}

/** The module factory `tessera_canvas.js` exports. */
type ModuleFactory = () => Promise<WasmModule & CanvasModule>;

export interface CanvasSessionOptions {
  elements: CanvasPaneElements;
  /**
   * What Run draws: the whole project and its entry, not the textarea's value.
   */
  source: ProjectReader;
  setStatus: StatusReporter;
  /** The toolbar's current `{width, height, dpr}`; a canvas run must not be stuck at 320x240. */
  environment: () => { width: number; height: number; dpr: number };
  /**
   * The toolbar's simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` |
   * `tabletop`. It is a layout input, so a change draws again rather than waiting for a Run.
   */
  foldPosture: () => string;
  /**
   * The selected skin's safe area (issue #171's skins slice), or `null` for no skin.
   *
   * It is a reader rather than a value for the same reason `environment` is: the toolbar can move
   * between two applies, and the session is created once. The insets are what the engine pads a
   * `SafeArea` by, so a skin change is a layout pass (`tessera_canvas_set_safe_area`) and not only a
   * repaint - `designer-studio-v1.md` section 5.1's "the skin must never change the engine's logical
   * size behind its back".
   */
  safeArea: () => SkinSafeArea | null;
  /** The component the person picked in the preview's dropdown; `""` is the entry's first. */
  component: () => string;
  /** The entry's declared components and the one previewed, after every successful apply. */
  onComponents: (names: string[], chosen: string) => void;
  /** Reports readiness to the page's shared Run button. */
  onReady: (ready: boolean) => void;
  /**
   * Applies a viewport a drag-resize reached (issue #690 slice b), or absent when the page has no
   * toolbar to tell.
   *
   * The session could apply the size itself through `_tessera_canvas_set_viewport`, but then the
   * toolbar would be stale: an orientation swap or a profile picker afterwards would snap back to a
   * size nobody chose, and the device chip would name a device the canvas is not showing. Going
   * through the page's own control handler keeps one owner of "what size is the preview", and the
   * session hears about it again through `onEnvironmentChange` like any other toolbar move.
   */
  onResize?: (width: number, height: number) => void;
  /**
   * The page's half of the style commit round trip (issue #585): the file buffers, the editor and the
   * live-edit trigger. Without it a commit cannot be applied, and the inspector's commit says so.
   */
  commit?: CommitBridge;
  /** Where Undo/Redo learns about this session's override stack (issue #585, rule D3). */
  editing?: OverrideDomainHost;
}

export interface CanvasSession extends ViewSession {
  readonly id: "canvas";
  readonly pane: CanvasPane;
  /**
   * The picker overlay (issue #171's picker slice). Off by default; the pane's toggle arms it, and
   * while armed a pointer move over the canvas outlines the node under it and names it.
   */
  readonly picker: CanvasPicker;
  /**
   * The style inspector (issue #171, designer-protocol-v1 section 9.1). Off by default; the pane's
   * toggle arms it, and while armed a click on the canvas shows that node's computed style, matched
   * rules and box. It consumes the click it inspects, which is why it is not on by default.
   */
  readonly inspector: CanvasInspector;
  /**
   * The computed style of one node, as `tessera_canvas_style` reports it, or `null` when no device
   * exists yet (issue #849).
   *
   * This is the seam a browser check needs to assert what the frame is *made of*. The probes used to
   * read the canvas element's screenshot and average its colour, which headless Chromium cannot do: it
   * does not composite a presented WebGPU frame, so `screenshot()` comes back black even while the pane
   * reports `drew N draw call(s)`. A style report is the same information without the compositor -- it
   * is what the engine resolved and painted with.
   */
  readonly styleInfo: (nodeRef: string) => CanvasStyleReport | null;
  /**
   * The automation id under one canvas-local point (issue #849), or `null` with no device.
   *
   * `styleInfo` needs an id and a check has no way to guess one, so this is its companion: a check
   * asks "what is at the centre of the canvas" and then "what style did that node resolve". Both are
   * queries (`_tessera_canvas_pick` and `_tessera_canvas_style`), so neither moves `:hover` nor costs
   * a frame.
   */
  readonly pickAt: (x: number, y: number) => { found: boolean; automationId: string } | null;
}

export function createCanvasSession(options: CanvasSessionOptions): CanvasSession {
  const pane = createCanvasPane(options.elements, options.setStatus);
  const { canvas } = options.elements;
  const source = options.source;
  // The module factory, or null before it loads. `gpu` holds the device the page acquired.
  let factory: ModuleFactory | null = null;
  let gpu: {
    device: GPUDevice;
    queue: GPUQueue;
    module: WasmModule & CanvasModule;
    context: GPUCanvasContext;
  } | null = null;
  // Set when a frame has been requested and not yet drawn, so a burst of changes makes one frame.
  let framePending = false;
  // The dpr the canvas was last sized for; a change resizes it (a real, drawable event).
  let lastDpr = 0;
  // Cleared while the view is hidden, so a hidden pane never draws.
  let visible = false;

  // The picker overlay (issue #171). It asks the module a question and draws a `div`; it never
  // dispatches, so arming it cannot move `:hover` or cost a frame. `module()` reads `gpu` lazily
  // because the picker is created before the first apply has acquired a device.
  const picker = createCanvasPicker({
    elements: {
      overlay: options.elements.pickOverlay ?? document.createElement("div"),
      canvas,
      toggle: options.elements.pickToggle,
    },
    module: () => (gpu !== null ? gpu.module : null),
    // `requestedViewport` takes the reader, not a reading: the toolbar can move between two picks.
    viewport: () => requestedViewport(options.environment),
    setStatus: options.setStatus,
  });

  // A pointer move over the canvas is the picker's only input. It is registered once, on the canvas
  // itself, and it does nothing while the picker is disarmed - so the preview's own pointer path
  // (which is the engine's, not this page's) is unaffected either way.
  canvas.addEventListener("pointermove", (event) => {
    picker.move(event.clientX, event.clientY);
  });
  // The pointer leaving the canvas is the same fact as a move over no node: the outline goes away
  // rather than staying over the last node the pointer touched.
  canvas.addEventListener("pointerleave", () => {
    picker.hide();
  });

  // The style inspector (issue #171, designer-protocol-v1 section 9.1). It is armed by its own
  // toggle and consumes the click it inspects, so a visitor who never opens it keeps a fully
  // interactive canvas. The listener is registered in the capture phase and stops the event when the
  // inspector took it: the engine's pointer path must not also see a press that was meant to select
  // a node, or the click would both inspect and activate the control under it.
  const overrides = createCanvasOverrides({
    module: () => (gpu !== null ? gpu.module : null),
    commit: options.commit,
    editing: options.editing,
    source,
    requestFrame: () => requestFrame(),
  });
  const inspector = createCanvasInspector({
    elements: {
      panel: options.elements.inspectPanel ?? document.createElement("div"),
      body: options.elements.inspectBody ?? document.createElement("div"),
      toggle: options.elements.inspectToggle,
      canvas,
    },
    module: () => (gpu !== null ? gpu.module : null),
    overrides: overrides.client,
    commitHost: overrides.host,
    stylesheets: () =>
      source()
        .stylesheets.map((sheet) => sheet.path)
        .sort(),
    viewport: () => requestedViewport(options.environment),
    requestFrame: () => requestFrame(),
    setStatus: options.setStatus,
  });
  canvas.addEventListener(
    "pointerdown",
    (event) => {
      if (inspector.click(event.clientX, event.clientY)) {
        // The click was the inspector's: it must not reach the engine, or the control under the
        // pointer would activate as well as being inspected.
        event.preventDefault();
        event.stopPropagation();
      }
    },
    { capture: true },
  );

  // The drag handles on the canvas (issue #690 slice b). A drag is a burst, so the layer records the
  // latest pointer position and this session applies one step per animation frame through the page's
  // control handler (`onResize`), plus one settled step on pointer-up. The arithmetic and the limits
  // are `canvas-resize.ts` and `canvas-viewport.ts`, so a drag cannot reach a size a toolbar move could
  // not - and the readout is computed from the same clamp the engine call uses.
  const resizeLayer = createCanvasResize({
    readout: document.getElementById("canvas-resize-readout"),
    // A pointer travels in CSS pixels of the canvas' layout box; the viewport is in the same units
    // at zoom 1, and the pane's zoom scales those CSS pixels. `cssZoomOf` is the ratio the picker
    // and the pointer path already use, so a drag at 68% moves the right distance.
    // The pane's zoom: the scale its CSS transform puts on the stage. It is read from the stage
    // rather than the canvas, because a transform scales an element's rect and its layout box alike,
    // so the canvas' own ratio is always 1. `useCanvasZoom` is the one owner of the transform; the
    // stage, when there is none, means 1.
    scale: () => {
      const stage = canvas.closest<HTMLElement>(".canvas-zoom-stage");
      if (stage === null) {
        return 1;
      }
      const zoom = cssZoomOf(stage.getBoundingClientRect().width, stage.offsetWidth);
      return Number.isFinite(zoom) && zoom > 0 ? zoom : 1;
    },
    current: () => requestedViewport(options.environment),
    apply: (width, height) => {
      options.onResize?.(width, height);
    },
    setStatus: options.setStatus,
  });

  // Sizes the backing store and remembers the dpr it used, so a resize can tell a real change from noise.
  // `clamped` (issue #690) says a limit reduced the request, so the caller can name the device it got.
  function resize(): { dpr: number; width: number; height: number; clamped: boolean } {
    const sized = sizeToDevicePixels(canvas, requestedViewport(options.environment));
    lastDpr = sized.dpr;
    return sized;
  }

  /**
   * Hands the selected skin's safe area to the running session (issue #171's skins slice).
   *
   * **Why the engine has to hear about it.** `designer-studio-v1.md` section 5.1 says a skin's insets
   * are "passed to the viewer as `safeAreas` in `viewport.set`, exactly as `WindowMetrics::insets`
   * would arrive from the HAL", and that "the skin must never change the engine's logical size behind
   * its back". A page-only overlay would draw a phone's status bar over a tree laid out as if the bar
   * were not there - a skin that changes the picture without changing the layout. This call is what
   * makes the `SafeArea` padding and every box follow the skin, through the same
   * `hal::resolve_safe_area()` the native app runs.
   *
   * `changed` is the module's own answer to "did a layout pass run", so a skin that is already in
   * force costs nothing and a skin change owes exactly one frame. A module built before this export
   * simply has no `_tessera_canvas_set_safe_area`; the session then reports that the skin's insets
   * were not applied rather than pretending they were, which is the honest degraded state.
   */
  function applySafeArea(): { ok: boolean; changed: boolean } {
    if (gpu === null) {
      return { ok: true, changed: false };
    }
    const area = options.safeArea();
    const insets = area?.insets ?? { top: 0, right: 0, bottom: 0, left: 0 };
    const cutout = area?.cutout ?? null;
    if (typeof gpu.module._tessera_canvas_set_safe_area !== "function") {
      if (area !== null) {
        pane.setStatus("this build cannot apply a skin's insets", "warning");
      }
      return { ok: true, changed: false };
    }
    try {
      const report = JSON.parse(
        callExport(gpu.module, "_tessera_canvas_set_safe_area", [], [
          insets.left,
          insets.top,
          insets.right,
          insets.bottom,
          cutout?.x ?? 0,
          cutout?.y ?? 0,
          cutout?.width ?? 0,
          cutout?.height ?? 0,
        ]),
      ) as { changed?: boolean; error?: string };
      if (report.error !== undefined && report.error !== "") {
        // A refused safe area (a negative inset) is named, and the picture already there stays.
        pane.setStatus("skin not applied: " + report.error, "warning");
        return { ok: false, changed: false };
      }
      return { ok: true, changed: report.changed === true };
    } catch (error) {
      pane.showFailed(String(error));
      return { ok: false, changed: false };
    }
  }
  // Draws the current source. Called from a `requestAnimationFrame` callback, never from the input handler,
  // so a fast typist cannot make the paint path reentrant and the canvas updates in step with the display.
  //
  // **Two module calls, not one, since #444.** The old path was `tessera_canvas_frame`, which evaluates
  // and paints in one stateless call -- so every keystroke re-evaluated from scratch, the state cells
  // were gone with the tree, and a failed parse made the module `clear_canvas` and hand back an error:
  // a blank rectangle exactly while a developer was mid-word. Now the session *applies* the source
  // (`tessera_canvas_apply`: evaluate, carry state, report whether a frame is owed) and then *draws*
  // (`tessera_canvas_draw`: paint the running tree, never re-evaluate). A source that does not evaluate
  // leaves the running tree in the module, so `draw` either is not called at all (no frame owed) or
  // paints the tree that is still there.
  function drawFrame(): void {
    framePending = false;
    if (factory === null || gpu === null || !visible) {
      return;
    }
    const sized = resize();
    // The DPR the backing store was actually sized for, not the one asked for: the limits may have
    // reduced it (issue #690), and the layout side must be handed the same scale the paint side is.
    const dpr = sized.dpr;
    options.elements.statDpr.textContent = String(dpr);
    if (sized.clamped) {
      // A clamp is named, never silent: the developer asked for a size that is not legal, and the pane
      // says which device it got instead of showing their number over a different one.
      pane.setStatus(
        `viewport clamped to ${Math.round(sized.width / dpr)}x${Math.round(sized.height / dpr)} ` +
          `at DPR ${dpr}`,
        "warning",
      );
    }
    // The layout viewport is the LOGICAL rectangle, and `width`/`height` above are the device-pixel
    // backing store. Passing the backing store as the viewport would lay the tree out at
    // `logical * dpr` and then scale it into the canvas - a 390x844 phone at DPR 2 would lay out at
    // 780x1688 and show the top-left quarter. The C ABI takes both, separately, for this reason.
    const viewport = requestedViewport(options.environment);
    const foldPosture = options.foldPosture();
    const target = source();
    // The paint extent is the backing store the page just sized, NOT the logical rectangle: the
    // canvas texture the page presents is that size, and asking for another would draw into a
    // mismatched target (issue #690 - the two used to differ only by a DPR rounding and were
    // therefore interchangeable; a clamped DPR makes them differ, so the honest value is passed).
    const extent = { width: sized.width, height: sized.height };
    let applied: CanvasApplyReport;
    try {
      // The project is staged first (the module evaluates what is staged, from its entry), then applied
      // with the component, the fold posture and the logical viewport and DPR. Applying and painting are
      // separate steps so a failed apply cannot clear the picture. The author's CSS rides with the staged
      // project: only the sheets the entry's import graph reaches apply.
      stageProject(gpu.module, target);
      applied = JSON.parse(
        callExport(gpu.module, "_tessera_canvas_apply", [options.component(), foldPosture], [
          viewport.width,
          viewport.height,
          dpr,
        ]),
      ) as CanvasApplyReport;
    } catch (error) {
      overrides.evaluated(false);
      pane.showFailed(String(error));
      options.onReady(true);
      return;
    }
    // The apply failed: the module kept the running evaluation, so nothing is drawn and the last good
    // frame stays on the canvas. The reason goes to the pane's status line, never to a blank surface.
    if (applied.error !== "") {
      overrides.evaluated(false);
      options.onComponents(applied.components ?? [], applied.component ?? "");
      pane.showNotApplied(applied.error);
      options.onReady(true);
      return;
    }
    // The state row follows every successful apply, `changed` or not: a cell that was carried is the
    // point of the edit, and it is the one fact a developer wants to see *while* the picture is still
    // the previous one (a reused apply reports it too).
    pane.clearNotApplied();
    pane.renderState(applied.state ?? []);
    options.onComponents(applied.components ?? [], applied.component ?? "");
    // The source evaluated (issue #585): a commit waiting for it may settle, and the text the module now
    // holds is what a later patch's offsets are measured in.
    overrides.staged(target);
    const settled = overrides.evaluated(true);
    if (settled !== null) {
      inspector.showOutcome(settled);
    }
    // The skin's safe area, applied AFTER the evaluation and BEFORE the frame (issue #171's skins
    // slice). It is a layout pass of its own (`tessera_canvas_set_safe_area`), so the boxes the next
    // `draw` paints already carry the skin's padding - no frame of stale insets. It runs on every
    // apply rather than only on a skin change because a Run re-evaluates the tree at the session's
    // current metrics, and a skin picked before the first Run must reach that first frame.
    const safe = applySafeArea();
    if (!safe.ok) {
      options.onReady(true);
      return;
    }
    // No frame owed: the source is byte-identical to what is painted (a debounce that fired on an
    // unchanged buffer, or a reuse after a DPR-resize that changed nothing) AND the skin moved
    // nothing. Drawing would repaint identical pixels, which is the idle rule this view exists to
    // honour. A skin change alone does owe a frame: it moved every box.
    if (!applied.changed && !safe.changed) {
      options.onReady(true);
      return;
    }
    let report: CanvasFrameReport;
    try {
      // One texture-view reference per frame: the module takes ownership and releases it, and the canvas
      // texture it names is only valid for this frame, so it is fetched here and not cached.
      const image = gpu.context.getCurrentTexture().createView();
      const handles = gpu.module.WebGPU;
      if (!handles) {
        throw new Error("the module dropped its WebGPU handle table");
      }
      const viewPtr = handles.importJsTextureView(image);
      // `_tessera_canvas_draw` takes no strings: the tree it paints is the one the apply above left in the
      // module, which already carries the project's stylesheets. `callExport` hands the export
      // `(...pointers, ...numbers)`.
      report = JSON.parse(
        callExport(gpu.module, "_tessera_canvas_draw", [], [
          extent.width,
          extent.height,
          viewPtr,
        ]),
      ) as CanvasFrameReport;
    } catch (error) {
      pane.showFailed(String(error));
      options.onReady(true);
      return;
    }
    pane.renderReport(report);
    // The frame moved the boxes the outline was drawn for, so the outline is re-asked rather than
    // left over a node that may have moved (issue #171). A pick is a query: this costs no frame.
    picker.refresh();
    // The inspector's panel describes a node of the tree this frame just painted, so it is re-asked
    // at the same point: a box that moved must not be shown at its old coordinates.
    inspector.refresh();
    options.onReady(true);
  }

  // Coalesces any number of change notifications into one frame on the next display refresh.
  function requestFrame(): void {
    if (framePending) {
      return;
    }
    framePending = true;
    globalThis.requestAnimationFrame(drawFrame);
  }

  async function loadModule(): Promise<void> {
    if (factory !== null) {
      return;
    }
    try {
      const imported = (await import(
        /* @vite-ignore */ wasmModuleUrl("tessera_canvas.js")
      )) as { default?: ModuleFactory };
      factory = imported.default ?? null;
      if (typeof factory !== "function") {
        throw new Error("the module did not export a factory");
      }
      options.onReady(true);
    } catch (error) {
      // The module is built by CI and published with the page; a checkout that has not run the
      // `wasm-webgpu` build has no artifacts next to these files, and saying that is more useful than an
      // empty canvas.
      pane.showUnsupported(
        "The WebAssembly canvas module did not load, so this view cannot draw. The module is built " +
          "by CI and published with the page; a checkout that has not run the `wasm-webgpu` build " +
          `has no artifacts next to these files. Detail: ${String(error)}`,
        "module",
      );
    }
  }

  // The device handover, once. Returns true when the view can draw.
  async function startGpu(): Promise<boolean> {
    if (factory === null) {
      return false;
    }
    // **The module is instantiated once, not once per Run.** A Wasm instance owns the GPU device, the
    // running session and the loaded font, so calling the factory again built a fresh instance whose
    // `g_device` is null by definition -- and the reuse branch below then swapped that device-less
    // instance in for the working one. Every Run after the first therefore reported "no WebGPU device"
    // (reproduced in a browser; a Viewport change only made it visible). One instance is also what the
    // rest of this file assumes: `gpu.module`, the font and the session are all per-instance state.
    let module: WasmModule & CanvasModule;
    if (gpu !== null) {
      module = gpu.module;
    } else {
      try {
        module = await factory();
      } catch (error) {
        pane.showFailed(`The preview module failed to instantiate: ${String(error)}`);
        return false;
      }
    }

    try {
      // Both exports are required: `apply` is what an edit and the first automatic load call, `draw`
      // paints the running session, and `set_viewport` is how the toolbar changes the device without
      // re-evaluating (issue #436).
      requireExports(module, [
        "_tessera_canvas_apply",
        "_tessera_canvas_draw",
        "_tessera_canvas_set_viewport",
        "_tessera_webgpu_set_device",
      ]);
    } catch (error) {
      pane.showUnsupported(String(error), "module");
      return false;
    }

    // A device already acquired is reused. A lost device is cleared in the handler below, which is the one
    // case that re-acquires.
    if (gpu === null) {
      const acquired = await acquireDevice();
      if (acquired.error !== undefined) {
        // No navigator.gpu, adapter or device: the browser's side of the handover.
        pane.showUnsupported(acquired.error, "browser");
        return false;
      }
      const handover = handOverDevice(module, acquired.device, acquired.queue, canvas);
      if (handover.error !== undefined) {
        // The browser gave a device; the module could not take it (or the canvas had no context).
        pane.showUnsupported(handover.error, "module");
        return false;
      }
      gpu = { device: acquired.device, queue: acquired.queue, module, context: handover.context };
      // A lost device is the one case that must draw again without a user action: the next Run would
      // otherwise hand the module a device the browser has already dropped.
      void acquired.device.lost.then((info: GPUDeviceLostInfo) => {
        gpu = null;
        pane.showUnsupported(
          `The WebGPU device was lost: ${info.reason || "unknown"}. Open the Canvas view again to ` +
            "reacquire it.",
        );
      });
    }

    resize();
    return true;
  }

  /**
   * Hands the module its font, once per device (issue #387). A failure is **not** fatal: the preview then
   * paints boxes only and the pane says so. The steps live in `canvas-font.ts`.
   */
  async function loadFont(module: WasmModule & CanvasModule): Promise<string | null> {
    return loadFontFor(module as never, (family) => pane.setFontState(family));
  }

  // **Restart app** (issue #444): a cold start. The module's session is released, so the next frame
  // evaluates from nothing, takes no carried state and every cell starts at its initializer -- the same
  // thing a page reload or a launch of the app does. This is the only path that discards state; an
  // edit never does (`apply`).
  async function run(): Promise<void> {
    if (factory === null) {
      return;
    }
    options.onReady(false);
    pane.setStatus("restarting", null);
    // The reasoning behind `hideStates` here and not on an apply: a restart is an explicit, expected
    // cold start, so replacing the pane's content is honest. A live edit must never do it (#444: the
    // canvas cannot blank while typing), which is why `apply` below touches no state but the status.
    const ready = await startGpu();
    if (!ready) {
      options.onReady(true);
      return;
    }
    // The font, after the device: the atlas needs GPU page textures, so `set_font` refuses before a
    // device exists. A failed fetch degrades the preview instead of failing it.
    if (gpu !== null) {
      const fontError = await loadFont(gpu.module);
      if (fontError !== null) {
        // Named, not silent: a designer must be able to tell "this source has no text" from "the font
        // did not load", and the status line is where the pane already reports that class of fact.
        pane.setStatus("font unavailable; text is not drawn", "warning");
        pane.setFontState(null, fontError);
      }
    }
    if (gpu !== null) {
      // The cold start itself: `tessera_canvas_reset` drops the running evaluation, so the draw that
      // follows evaluates the source afresh with nothing carried. It returns a small document (the
      // released session's frame count) rather than void, because `callExport` reads every export as a
      // returned document -- a void export would surface as "returned no document", a crash-shaped
      // message for a successful restart.
      callExport(gpu.module, "_tessera_canvas_reset");
      // The state row belongs to the evaluation that was just released, so it goes with it: after a
      // restart the count is whatever the initializer says, and showing the old value until the next
      // frame lands would be the one stale reading on the page.
      pane.renderState([]);
      // The outline named a node of the evaluation that was just released (issue #171).
      picker.hide();
      // The inspector's panel described a node of the same evaluation.
      inspector.hide();
      // Overrides live in the released session (designer-protocol-v1 section 9.2): they and their history go.
      overrides.ended();
    }
    requestFrame();
  }

  /**
   * Applies an edit without resetting anything (issue #444).
   *
   * This is the hot path the debounced trigger calls ~150 ms after a keystroke, and the same thing
   * `Ctrl+Enter` calls immediately. It must never take a destructive action: the frame it requests
   * either paints the re-evaluated tree (state carried by the engine's own rule) or, when the source
   * does not compile, paints nothing at all and leaves the last good picture plus a status sentence.
   *
   * It also must not *cost* a frame when nothing changed, which is the module's answer (`changed`),
   * not a decision made here.
   */
  async function apply(): Promise<void> {
    if (factory === null) {
      return;
    }
    // The device is acquired here, lazily, on the first apply -- never on page load. A session that has
    // not drawn yet has no device, and an apply is the first thing that needs one.
    const ready = await startGpu();
    if (!ready) {
      return;
    }
    if (gpu !== null && lastDpr === 0) {
      // The font is handed over once per device, and the first apply is where a device first exists.
      // `lastDpr === 0` is exactly "this session has not resized yet", i.e. no frame has been drawn, so
      // this runs on the first apply and never again -- and never on a restart, which the `run` path
      // above has already handled.
      const fontError = await loadFont(gpu.module);
      if (fontError !== null) {
        pane.setFontState(null, fontError);
      }
    }
    requestFrame();
  }

  // Pointer input (issue #388): hover, press and click on the canvas go to the engine, which owns what they
  // mean; a reported change is one coalesced frame. Not while hidden or before a device exists.
  attachCanvasPointer({
    canvas,
    viewport: () => requestedViewport(options.environment),
    onChanged: requestFrame,
    send: (kind, x, y, button) =>
      gpu === null || !visible || typeof gpu.module._tessera_canvas_pointer !== "function"
        ? null
        : (JSON.parse(callExport(gpu.module, "_tessera_canvas_pointer", [kind], [x, y, button])) as { changed: boolean }),
  });

  // Keyboard input and focus (issue #388): the canvas is a focus stop, and the engine owns what a key means.
  attachCanvasKeyboard({
    canvas,
    onChanged: requestFrame,
    send: (kind, key, modifiers, repeat) =>
      gpu === null || !visible || typeof gpu.module._tessera_canvas_key !== "function"
        ? null
        : (JSON.parse(callExport(gpu.module, "_tessera_canvas_key", [kind, key], [modifiers, repeat ? 1 : 0])) as {
            changed: boolean;
            handled?: boolean;
            left?: boolean;
          }),
  });

  // A device-pixel-ratio change (a window dragged to another monitor, a browser zoom) changes the backing
  // store, which is a real reason to draw again. A resize alone does not: the CSS box follows the toolbar,
  // and the toolbar has its own change hook.
  globalThis.addEventListener("resize", () => {
    const dpr = globalThis.devicePixelRatio > 0 ? globalThis.devicePixelRatio : 1;
    if (gpu !== null && visible && dpr !== lastDpr) {
      requestFrame();
    }
  });

  // A hidden tab draws nothing; becoming visible again is the one event that restores an on-demand canvas
  // that was not painted while it was hidden.
  document.addEventListener("visibilitychange", () => {
    if (gpu !== null && visible && !document.hidden) {
      requestFrame();
    }
  });

  return {
    id: "canvas",
    pane,
    picker,
    inspector,
    /** First open of the view: decide the unsupported state and import the module (no device yet). */
    load: async () => {
      visible = true;
      if (!navigator.gpu) {
        // Decided from the API's presence, which needs no download: a browser without WebGPU is told so
        // without paying for a module it cannot use.
        pane.showUnsupported(
          "This browser has no navigator.gpu. WebGPU is available in Chrome/Edge 113+, Safari 18+ " +
            "and Firefox 141+ (on supported hardware). The Scene tab needs no GPU at all.",
        );
        return;
      }
      await loadModule();
      if (factory !== null) {
        pane.setStatus("ready", null);
      }
    },
    /** The **Restart app** button's action: an explicit cold start, state reset. */
    run,
    /**
     * An edit was applied (issue #444): the debounced trigger, `Ctrl+Enter`, and the first load.
     *
     * It is the same entry point for all three on purpose. The first automatic apply is what acquires
     * the device, so "the preview starts itself once the module and the device are ready" costs no
     * separate code path -- and a page whose Canvas tab has never been opened has no session to apply
     * to, so a visitor who never looks at Canvas still takes no GPU.
     */
    apply,
    /** "Load sample": the canvas draws whatever the shared editor holds, so only a live device redraws. */
    loadSample: () => {
      if (gpu !== null && visible) {
        requestFrame();
      }
    },
    /** The pane became (in)visible: stop drawing while hidden, redraw once when shown again. */
    setVisible: (shown: boolean) => {
      visible = shown;
      if (!shown) {
        // A hidden pane shows no outline: the picker's overlay belongs to the canvas it covers.
        picker.hide();
        // The inspector's panel belongs to the same canvas.
        inspector.hide();
      }
      if (shown && gpu !== null) {
        requestFrame();
      }
    },
    /**
     * The panel was closed: stop drawing and release the GPU device (studio-workbench-layout-v1.md
     * section 3.3). The session is not reused; reopening the panel creates a new one.
     */
    dispose: () => {
      visible = false;
      // The grips detach first: a drag in flight must not call `apply` on a disposed session.
      resizeLayer.dispose();
      picker.hide();
      inspector.hide();
      if (gpu !== null) {
        try {
          gpu.device.destroy();
        } catch {
          /* an already lost device: nothing left to release */
        }
      }
      gpu = null;
      factory = null;
      // After the device is released, so a listener of the routing store can never keep it alive.
      overrides.dispose();
    },
    /**
     * The toolbar moved: on this view the viewport, the DPR and the fold posture are what the canvas is
     * sized and laid out for, and the layout pass happens inside the engine call this makes (issues #408
     * and #436). **A viewport change is not a reason to re-evaluate.** `_tessera_canvas_set_viewport`
     * re-lays the running evaluation out in place — no re-run, `@media` re-resolved only when the size
     * crosses a breakpoint — so `state::count`, hover and focus survive a preset or orientation swap.
     */
    onEnvironmentChange: () => {
      if (gpu === null || !visible) {
        return;
      }
      resize();
      const viewport = requestedViewport(options.environment);
      try {
        const report = JSON.parse(
          callExport(gpu.module, "_tessera_canvas_set_viewport", [], [
            viewport.width,
            viewport.height,
            viewport.dpr,
          ]),
        ) as CanvasViewportReport;
        if (report.error !== "") {
          // A refused viewport (a degenerate size) is named, and the picture already there stays.
          pane.setStatus("viewport not applied: " + report.error, "warning");
          return;
        }
        if (report.clamped === true) {
          // The engine reduced the request to fit its limits (issue #690). Name the device it applied,
          // from the report's own numbers, rather than showing the toolbar's.
          pane.setStatus(
            `viewport clamped to ${Math.round(report.width)}x${Math.round(report.height)} ` +
              `at DPR ${report.dpr}`,
            "warning",
          );
        }
      } catch (error) {
        pane.showFailed(String(error));
        return;
      }
      // The skin is a layout input too (issue #171's skins slice), and it is applied in the same
      // handler so a toolbar move that changes both the size and the skin costs one frame rather
      // than two. The engine runs one layout pass per call, so the order is not observable: the
      // frame below paints the boxes both calls left.
      if (!applySafeArea().ok) {
        return;
      }
      // The viewport moved every box, so the outline is re-asked at the same point (issue #171).
      picker.refresh();
      // The inspector's panel is re-asked for the same reason: its box is in logical pixels.
      inspector.refresh();
      requestFrame();
    },
    /**
     * One platform event (issue #446). The engine owns what it means; the page redraws when the reply says
     * a layout input moved, and after a relaunch -- the engine ended the evaluation, so the next frame's
     * apply is the cold start, carrying the saved cells only when the reply says `restore`.
     */
    platformEvent: (request) => {
      if (gpu === null) {
        return unavailableReply(request.type, "the canvas preview has not started");
      }
      const reply = sendPlatformEvent(gpu.module, request);
      if (reply.ok && reply.relaunch) {
        // The released evaluation's state row, outline and inspector panel go with it.
        pane.renderState([]);
        picker.hide();
        inspector.hide();
        overrides.ended();
      }
      if (reply.ok && (reply.relaunch || reply.changed) && visible) {
        requestFrame();
      }
      return reply;
    },
    /**
     * The computed style of one node (issue #849): the automation seam a browser check uses instead of
     * reading the composited canvas.
     *
     * It is a **query**: `tessera_canvas_style` dispatches nothing, so asking it between two frames
     * cannot move `:hover`, start an animation or cost a draw. A page with no device answers `null`
     * rather than throwing, so a check can report "nothing drew yet" as a verdict.
     */
    styleInfo: (nodeRef: string) => {
      if (gpu === null) {
        return null;
      }
      try {
        return JSON.parse(callExport(gpu.module, "_tessera_canvas_style", [nodeRef], [])) as CanvasStyleReport;
      } catch {
        return null;
      }
    },
    /** The automation id at one canvas-local point (issue #849); see `pickAt`'s note. */
    pickAt: (x: number, y: number) => {
      if (gpu === null) {
        return null;
      }
      try {
        return JSON.parse(callExport(gpu.module, "_tessera_canvas_pick", [], [x, y])) as {
          found: boolean;
          automationId: string;
        };
      } catch {
        return null;
      }
    },
    /** A gallery pick changes what would be drawn; redraw only if a device already exists. */
    onSample: () => {
      if (gpu !== null && visible) {
        requestFrame();
      }
    },
  };
}
