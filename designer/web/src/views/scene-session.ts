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
import type { SourceReader, StatusReporter, ViewSession } from "../lib/views";
import { callExport, requireExports, type WasmModule } from "../lib/wasm-call";
import { createInputPane } from "./scene-input";
import { describeCss, cssProblem, readCssDiagnostics } from "./css-diagnostics";
import { DEFAULT_VIEWPORT, chooseExport } from "./scene-export";
import { noSceneBox, renderNodes, unavailableBox, type SceneDocument } from "./scene-render";

/** The module factory `tessera_scene_preview.js` exports. */
type ModuleFactory = () => Promise<WasmModule>;

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

/** The elements React committed for the Scene pane; every one of them is a ref. */
export interface SceneElements {
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
  /**
   * What Run compiles (issue #431): the workspace's compile target, not the textarea's current value.
   * A stylesheet tab being active does not make the stylesheet the source.
   */
  source: SourceReader;
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
  /** Reports readiness to the page's shared Run button. */
  onReady: (ready: boolean) => void;
}

export interface SceneSession extends ViewSession {
  readonly id: "scene";
}

export function createSceneSession(options: SceneSessionOptions): SceneSession {
  const { nodes, emptyState, editorHint, script, cells, runInput } = options.elements;
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
    source: options.source,
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
      // The source and its stylesheet come from one reader (issue #431's `SourceReader`, extended by #390):
      // the sheet is read when Run is pressed, exactly like the text it styles, so a session cannot lay the
      // tree out against a sheet the author no longer has on screen.
      const target = options.source();
      const sheet = target.stylesheet;
      const chosen = chooseExport(module, viewport, dpr, options.foldPosture(), sheet !== "");
      // The argument order every export in this module shares: the pointers first (source, file,
      // component, then the posture and/or stylesheet when the chosen shape takes one), then the
      // numbers. `callExport` marshals the strings in order, so a string after a number would be
      // handed a stale integer.
      const strings = [target.text, target.file, ""];
      if (chosen.postureFirst) {
        strings.push(options.foldPosture());
      }
      if (chosen.styled) {
        strings.push(sheet);
      }
      const json = callExport(module, chosen.name, strings, chosen.numbers);
      emptyState.hidden = true;
      const document = JSON.parse(json) as SceneDocument;
      const count = renderNodes(nodes, document, (id) => input.append(id));
      // The stylesheet's own problems are reported beside the snapshot, not instead of it (issue
      // #390): the parser is tolerant, so a bad rule costs that rule and the tree still renders. When
      // the sheet is set but the artifact cannot apply it, say so rather than implying it was used.
      const css = await readCssDiagnostics(module, sheet, target.stylesheetFile);
      if (count === 0) {
        nodes.replaceChildren();
        nodes.append(noSceneBox(document.error));
        setStatus("no scene", "warning");
      } else {
        setStatus(`${count} node(s)${describeCss(css)}`, cssProblem(css) ? "warning" : "ok");
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
    /**
     * "Load sample" on this view, split across the page (issue #431): the page writes `sampleSource` into
     * the entry file, and this runs the view's own part -- its three-click demo script over that source.
     */
    sampleSource: SAMPLE,
    loadSample: () => {
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