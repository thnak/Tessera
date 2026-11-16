// SPDX-License-Identifier: MIT

// The Diagnostics view's session (P1, issue #165; the React host is issue #422).
//
// It compiles the shared source with `tessera_diagnostics`, the same compiler the native CLI runs, and
// lists the diagnostics with their source spans. The rendering is `diagnostics-render.ts`; this file is
// the Wasm lifecycle and the pane's status line.
//
// It never trusts that the module loaded: a missing or failed module leaves the view in an explicit
// "preview unavailable" state, because the Wasm artifacts are built by CI and a local checkout that has
// not built them must say so rather than show a blank pane. The module is imported on first use
// (`load()`), so the diagnostics bundle is the only one on the first-paint path -- and it is the one
// every visitor needs, because it is the default view.
//
// A diagnostics run has no viewport and no DPR: the compiler is handed a file name and nothing else.
// The toolbar's environment is *carried* by the share link on this view but not applied here, and the
// view says so rather than reporting a rectangle the compiler never used.
//
// **What React owns here.** The pane's header, status line, list element and empty state are React's
// (rendered childless, in `regions/ViewPanes.tsx`). This session fills the list and the empty state and
// writes the status text; it never creates or removes a node React rendered. That is the ADR-054
// decision 3 split: React draws the chrome, the engine draws the preview.

import { wasmModuleUrl } from "../lib/module-url";
import { callExport, requireExports, type WasmModule } from "../lib/wasm-call";
import type { StatusReporter, ViewSession } from "../lib/views";
import { renderDiagnostics, type DiagnosticsDocument } from "./diagnostics-render";
import { unavailableBox } from "./dom";



/** The file name the compiler is handed. A link may carry another (the sample's path). */
const FILE = "ui/Preview.tsx";

/** The module factory `tessera_diagnostics.js` exports (Emscripten `MODULARIZE`+`EXPORT_ES6`). */
type ModuleFactory = () => Promise<WasmModule>;

/** The elements React committed for this pane; every one of them is a ref. */
export interface DiagnosticsElements {
  source: HTMLTextAreaElement;
  status: HTMLElement;
  list: HTMLElement;
  emptyState: HTMLElement;
}

export interface DiagnosticsSession extends ViewSession {
  readonly id: "diagnostics";
}

/**
 * Creates the diagnostics session.
 *
 * `elements` is read once: the nodes are stable for the page's lifetime because React renders them
 * childless and never conditionally. `setStatus` is the pane's own status line, and `onReady` tells the
 * page whether the shared Run button should be enabled.
 */
export function createDiagnosticsSession(options: {
  elements: DiagnosticsElements;
  setStatus: StatusReporter;
  onReady: (ready: boolean) => void;
}): DiagnosticsSession {
  const { source, list, emptyState } = options.elements;
  const setStatus = options.setStatus;

  // The module factory, or null when the artifacts are absent or refused to start.
  let factory: ModuleFactory | null = null;

  function showUnavailable(detail: string): void {
    list.replaceChildren();
    emptyState.hidden = true;
    list.append(
      unavailableBox(
        "Preview unavailable in this build",
        "The WebAssembly compiler did not load, so the diagnostics view cannot validate TSX. The " +
          "module is built by CI and published with the page; a checkout that has not run the " +
          "`wasm-compiler` build has no artifacts next to these files.",
        detail,
      ),
    );
    setStatus("module unavailable", "warning");
  }

  function report(counts: { total: number; errors: number; warnings: number }): void {
    if (counts.total === 0) {
      setStatus("no diagnostics", "ok");
      return;
    }
    const parts: string[] = [];
    if (counts.errors > 0) {
      parts.push(counts.errors + (counts.errors === 1 ? " error" : " errors"));
    }
    if (counts.warnings > 0) {
      parts.push(counts.warnings + (counts.warnings === 1 ? " warning" : " warnings"));
    }
    setStatus(parts.join(", "), counts.errors > 0 ? "error" : "warning");
  }

  async function run(): Promise<void> {
    if (!factory) {
      return;
    }
    options.onReady(false);
    setStatus("compiling...");
    try {
      // One module instance per view: the compiler keeps no state between calls that the page needs,
      // and re-instantiating per Run would re-download and re-initialise the heap.
      const module = await factory();
      // The marshalling and the two release paths live in `lib/wasm-call.ts`: the three views share the
      // same C ABI shape, so the lifetime rule is written once instead of three times.
      const json = callExport(module, "_tessera_diagnostics_json", [source.value, FILE]);
      report(
        renderDiagnostics(
          list,
          emptyState,
          JSON.parse(json) as DiagnosticsDocument,
          source,
        ),
      );
    } catch (error) {
      list.replaceChildren();
      emptyState.hidden = true;
      list.append(
        unavailableBox(
          "The compiler failed",
          String(error instanceof Error ? error.message : error),
        ),
      );
      setStatus("failed", "error");
    } finally {
      options.onReady(true);
    }
  }

  async function load(): Promise<void> {
    if (factory !== null) {
      return;
    }
    try {
      const imported = (await import(/* @vite-ignore */ wasmModuleUrl("tessera_diagnostics.js"))) as { default?: ModuleFactory };
      factory = imported.default ?? null;
      if (typeof factory !== "function") {
        throw new Error("the module did not export a factory");
      }
      // Instantiate once up front so a broken module fails visibly at load, not on the first Run, and
      // so a module built without the exports this view calls fails with a readable message rather
      // than as "x is not a function" later.
      requireExports(await factory(), ["_tessera_diagnostics_json"]);
      setStatus("ready");
      options.onReady(true);
    } catch (error) {
      // A missing artifact is the normal state of a checkout that has not built Wasm; `import()` of a
      // missing file rejects with a TypeError whose message names the URL.
      factory = null;
      options.onReady(false);
      showUnavailable(error instanceof Error ? error.message : String(error));
    }
  }

  return {
    id: "diagnostics",
    load,
    run,
    /** "Load sample" on this view: the compiler's zero-network default into the shared editor. */
    loadSample: () => {
      source.value = SAMPLE;
      source.focus();
    },
    /** A gallery pick: the text is already in the textarea, so only the status line changes. */
    onSample: (path: string) => setStatus(`loaded ${path}`, "ok"),
    /** This view has no layout, so an environment change is carried into the link and nothing more. */
    onEnvironmentChange: () => {},
    /** Only one view is on screen; the others never draw or re-layout, so this is a no-op here. */
    setVisible: () => {},
  };
}

/**
 * A small sample that is valid, so the first Run shows the empty-diagnostics path. The gallery's
 * samples are read from `samples/`; this stays as the zero-network default.
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
