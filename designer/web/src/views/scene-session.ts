// SPDX-License-Identifier: MIT

// The Scene view's session (P2 issue #166 and P4 issue #168; the React host is issue #422).
//
// It calls the same C exports the parity gates drive, parses the returned canonical documents, and
// renders the node list (path, kind, bounds, text) and the state cells after every scripted click
// (`scene-render.ts` renders the nodes; `scene-input.ts` renders the cells).
//
// The second toggle that made this view special: the viewport and DPR are **layout inputs**, so a
// toolbar change re-runs the layout rather than waiting for the next Run.
//
// **What a click does here.** The click is dispatched inside the module by the engine's own input
// router, so hit testing, pointer capture, click synthesis and capture/target/bubble propagation are the
// production code paths. The handler it reaches is the handler's TSX *preview body*, executed by the IR
// evaluator -- the same body `tests/parity/parity_runner.cpp` runs. It is not the developer's C++: that
// code is not linked into this module and a click cannot run it. A handler that takes a payload
// parameter (`(e: ClickEvent) =>`) is reported as `unrun`, because the evaluator has no record value to
// bind to it. The view states this rather than implying real handler execution.
//
// The theme is carried in the link but not consumed by the null-RHI snapshot (the module has no theme
// asset), and that is stated rather than implied. The module is imported when this view is first shown,
// never on the first paint: a visitor who only wants diagnostics never downloads it.

import { parseViewport } from "../lib/preview-state.js";
import { wasmModuleUrl } from "../lib/module-url";
import type { StatusReporter, ViewSession } from "../lib/views";
import { callExport, requireExports, type WasmModule } from "../lib/wasm-call";
import { createInputPane } from "./scene-input";
import { noSceneBox, renderNodes, unavailableBox, type SceneDocument } from "./scene-render";

/** The module factory `tessera_scene_preview.js` exports. */
type ModuleFactory = () => Promise<WasmModule>;

// The preview's default layout rectangle (wasm_scene_entry.cpp's kDefaultWidth/Height/DpiScale): the
// plain `tessera_scene_json` export lays the tree out at exactly this, so the sized export is only
// needed when the toolbar has moved off it.
const DEFAULT_VIEWPORT = { width: 320, height: 240 };
const DEFAULT_DPR = 1;

/**
 * The parity-demo-shaped sample: a static tree the preview evaluates, so the first Run shows a real
 * document instead of an unsupported-construct error. The button carries an explicit `automationId` so
 * the input script below names a node that exists without the reader having to look up a hash.
 */
export const SAMPLE = `export function Counter(): Node {
  const [count, setCount] = useState<int>(0);

  return (
    <View>
      <Text>Count: {count}</Text>
      <Button automationId="101" onClick={() => setCount(count + 1)}>Add</Button>
    </View>
  );
}
`;

const SAMPLE_SCRIPT = "click 101\nclick 101\nclick 101\n";

// The plain export is the parity gate's contract (byte-identical to the native CLI's 320x240 DPR 1) and
// is what a default run must keep calling; the sized one is for a moved toolbar. The fallback covers an
// artifact built before P5 landed next to this page.
function usesSizedExport(
  module: WasmModule,
  viewport: { width: number; height: number },
  dpr: number,
): boolean {
  const moved =
    viewport.width !== DEFAULT_VIEWPORT.width ||
    viewport.height !== DEFAULT_VIEWPORT.height ||
    dpr !== DEFAULT_DPR;
  return moved && typeof module._tessera_scene_json_sized === "function";
}

/**
 * The export this run should call, and why.
 *
 * `_tessera_scene_json_folded` (issue #408) is the only export that carries the fold posture, so a
 * foldable preview has to use it. A run with no posture keeps calling the plain export when nothing
 * else moved, because that byte-for-byte equality with the native CLI's default is what the parity
 * gate asserts; a moved viewport keeps using the sized export, and a fold takes precedence because
 * the sized one cannot express it.
 *
 * The `folded` export falls back to the sized/plain pair when the artifact predates it: a page
 * published against an older module then shows an unfolded preview and says so, rather than failing.
 */
function chooseExport(
  module: WasmModule,
  viewport: { width: number; height: number },
  dpr: number,
  posture: string,
): { name: string; numbers: number[]; extra: string[]; folded: boolean } {
  if (posture !== "" && typeof module._tessera_scene_json_folded === "function") {
    // `callExport` hands the trailing string arguments after the numeric ones, so the posture is
    // appended to the source triple: (source, file, component, w, h, dpr, posture).
    return {
      name: "_tessera_scene_json_folded",
      numbers: [viewport.width, viewport.height, dpr],
      extra: [posture],
      folded: true,
    };
  }
  if (usesSizedExport(module, viewport, dpr)) {
    return {
      name: "_tessera_scene_json_sized",
      numbers: [viewport.width, viewport.height, dpr],
      extra: [],
      folded: false,
    };
  }
  return { name: "_tessera_scene_json", numbers: [], extra: [], folded: false };
}

/** The elements React committed for the Scene pane; every one of them is a ref. */
export interface SceneElements {
  source: HTMLTextAreaElement;
  status: HTMLElement;
  nodes: HTMLElement;
  emptyState: HTMLElement;
  editorHint: HTMLElement;
  script: HTMLTextAreaElement;
  inputStatus: HTMLElement;
  cells: HTMLElement;
  runInput: HTMLButtonElement | null;
}

export interface SceneSessionOptions {
  elements: SceneElements;
  /** The pane's own status line (the node list's). */
  setStatus: StatusReporter;
  /** The input pane's status line (the state cells'). */
  setInputStatus: StatusReporter;
  /** The toolbar's current viewport and DPR, as layout inputs. */
  environment: () => { viewport: string; dpr: string };
  /**
   * The toolbar's simulated fold posture (issue #408): `""` for no fold, else `flat` | `book` |
   * `tabletop`. A layout input exactly like the viewport, so a change re-runs the layout rather than
   * waiting for the next Run, and the posture travels through the same `set_viewport` field the
   * native protocol and MCP use.
   */
  foldPosture: () => string;
  /** The source path the current sample is previewed under. */
  file: () => string;
  /** Reports readiness to the page's shared Run button. */
  onReady: (ready: boolean) => void;
}

export interface SceneSession extends ViewSession {
  readonly id: "scene";
}

export function createSceneSession(options: SceneSessionOptions): SceneSession {
  const { source, nodes, emptyState, editorHint, script, cells, runInput } = options.elements;
  const setStatus = options.setStatus;

  // The module factory, or null when the artifacts are absent or refused to start.
  let factory: ModuleFactory | null = null;

  function currentViewport(): { width: number; height: number } {
    try {
      return parseViewport(options.environment().viewport);
    } catch {
      return { ...DEFAULT_VIEWPORT };
    }
  }

  function currentDpr(): number {
    return Number(options.environment().dpr);
  }

  /**
   * Appends the live viewport to the page's editor hint.
   *
   * The hint element is the page's (one shared editor), and the page sets the per-view sentence when the
   * tab changes. On this view the viewport is a real layout input, so the number in that sentence has to
   * follow the toolbar -- otherwise the hint would say "the toolbar re-runs the layout" next to a
   * rectangle it no longer names. Every other view leaves the static sentence alone.
   */
  function describeEnvironment(): void {
    const base = editorHint.textContent?.split(" The viewport is ")[0] ?? "";
    const viewport = currentViewport();
    editorHint.textContent = `${base} The viewport is ${viewport.width}x${viewport.height} at DPR ${currentDpr()}.`;
  }

  const input = createInputPane({
    elements: { script, cells, button: runInput },
    source,
    file: options.file,
    request: async (name, args) => {
      if (factory === null) {
        throw new Error("the WebAssembly module is not loaded");
      }
      return callExport(await factory(), name, args);
    },
    setStatus: options.setInputStatus,
    setEnabled: (enabled) => {
      if (runInput) {
        runInput.disabled = !enabled;
      }
    },
  });

  async function run(): Promise<void> {
    if (factory === null) {
      return;
    }
    options.onReady(false);
    setStatus("running", null);
    try {
      const module = await factory();
      const viewport = currentViewport();
      const dpr = currentDpr();
      const file = options.file();
      const chosen = chooseExport(module, viewport, dpr, options.foldPosture());
      const json = callExport(
        module,
        chosen.name,
        [source.value, file, "", ...chosen.extra],
        chosen.numbers,
      );
      emptyState.hidden = true;
      const document = JSON.parse(json) as SceneDocument;
      const count = renderNodes(nodes, document, (id) => input.append(id));
      if (count === 0) {
        nodes.replaceChildren();
        nodes.append(noSceneBox(document.error));
        setStatus("no scene", "warning");
      } else {
        setStatus(`${count} node(s)`, "ok");
      }
    } catch (error) {
      nodes.replaceChildren();
      emptyState.hidden = true;
      nodes.append(unavailableBox("Preview failed", String(error)));
      setStatus("failed", "warning");
    } finally {
      options.onReady(true);
    }
  }

  async function load(): Promise<void> {
    if (factory !== null) {
      return;
    }
    try {
      const imported = (await import(
        /* @vite-ignore */ wasmModuleUrl("tessera_scene_preview.js")
      )) as { default?: ModuleFactory };
      factory = imported.default ?? null;
      if (typeof factory !== "function") {
        throw new Error("the module did not export a factory");
      }
      // Instantiate once so a module built without the exports this view calls fails at load with a
      // readable message, rather than as "x is not a function" on the first Run.
      requireExports(await factory(), ["_tessera_scene_json", "_tessera_input_json"]);
      options.onReady(true);
      if (runInput) {
        runInput.disabled = false;
      }
      describeEnvironment();
      // The view is being opened for the first time with the source already in place: laying it out
      // straight away is what makes the Scene tab show a snapshot rather than an empty pane.
      await run();
    } catch (error) {
      nodes.replaceChildren();
      emptyState.hidden = true;
      nodes.append(
        unavailableBox(
          "Preview unavailable in this build",
          "The WebAssembly scene module did not load, so the scene view cannot lay the source out. " +
            "The module is built by CI and published with the page; a checkout that has not run " +
            "the `wasm-webgpu` build has no artifacts next to these files.",
          String(error),
        ),
      );
      setStatus("module unavailable", "warning");
      options.onReady(false);
    }
  }

  return {
    id: "scene",
    /** Loads the Wasm module (and lays the current source out once) on the view's first open. */
    load,
    /** The Run button's action. */
    run,
    /** "Load sample" on this view: its own sample plus the three-click demo script. */
    loadSample: () => {
      source.value = SAMPLE;
      script.value = SAMPLE_SCRIPT;
      input.reset();
      if (factory !== null) {
        void run();
      }
    },
    /** Only one view is on screen: a hidden scene pane neither draws nor re-lays out. */
    setVisible: () => {},
    /** Re-runs the layout because the toolbar changed: on this view the toggles are real inputs. */
    onEnvironmentChange: () => {
      describeEnvironment();
      if (factory !== null) {
        void run();
      }
    },
    /** A gallery pick: the previous sample's cells belong to a document this one did not evaluate. */
    onSample: () => {
      input.reset();
      script.value = "";
      describeEnvironment();
      if (factory !== null) {
        void run();
      }
    },
  };
}
