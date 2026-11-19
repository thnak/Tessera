// SPDX-License-Identifier: MIT

// The Diagnostics view's session (P1, issue #165; the React host is issue #422; the multi-file workspace
// is issue #431).
//
// It compiles the source the workspace selects with `tessera_diagnostics`, the same compiler the native
// CLI runs, and lists the diagnostics with their source spans. The rendering is
// `diagnostics-render.ts`; this file is the Wasm lifecycle and the pane's status line.
//
// **What it compiles.** Before #431 this session read `#source.value`, which was the page's one textarea
// and therefore always the right text. Now the editor shows one project file at a time and Run compiles
// the *compile target* (`lib/project.ts`: the active source file, else the project entry), so the text
// and the file name both come from the page's `ProjectReader` -- and the file name is the canonical
// project path, which is what every `uiId` and every span is derived from.
//
// **What React owns here.** The pane's header, status line, list element and empty state are React's
// (rendered childless, in `regions/ViewPanes.tsx`). This session fills the list and the empty state,
// writes the status text, and mirrors the count into the page's status line through a ref. That is the
// ADR-054 decision 3 split: React draws the chrome, the engine draws the preview.

import type { CodeEditor, EditorMark } from "../editor/code-editor";
import { wasmModuleUrl } from "../lib/module-url";
import type { StatusReporter, ProjectReader, ViewSession } from "../lib/views";
import { callExport, requireExports, stageProject, type WasmModule } from "../lib/wasm-call";
import { renderDiagnostics, type Diagnostic, type DiagnosticsDocument } from "./diagnostics-render";
import { unavailableBox } from "./dom";

/** The module factory `tessera_diagnostics.js` exports (Emscripten `MODULARIZE`+`EXPORT_ES6`). */
type ModuleFactory = () => Promise<WasmModule>;

/** The elements React committed for this pane; every one of them is a ref. */
export interface DiagnosticsElements {
  status: HTMLElement;
  list: HTMLElement;
  emptyState: HTMLElement;
  /** The editor, for "show in source". The page decides whether it holds the compiled text. */
  editor: CodeEditor;
  /** An optional cell that receives the "N errors, M warnings" text (issue #431); the page uses the Problems store instead. */
  statusCount?: HTMLElement | null;
}

export interface DiagnosticsSession extends ViewSession {
  readonly id: "diagnostics";
}

/**
 * Creates the diagnostics session.
 *
 * `elements` is read once: the nodes are stable for the page's lifetime because React renders them
 * childless and never conditionally. `source` is the page's `ProjectReader`, `setStatus` is the pane's own
 * status line, and `onReady` tells the page whether the shared Run button should be enabled.
 */
/** The files the last diagnostics run underlined, so a file that became clean is cleared. */
const markedFiles = new Set<string>();

/** Underlines each diagnostic's span in its file's editor state (issue #517): the list stays, the span shows inline. */
function markEditor(editor: CodeEditor, document_: DiagnosticsDocument): void {
  const byFile = new Map<string, EditorMark[]>();
  for (const diagnostic of document_.diagnostics ?? []) {
    const span = diagnostic.span;
    if (!span || !span.file || typeof span.offset !== "number" || span.offset < 0) {
      continue;
    }
    const list = byFile.get(span.file) ?? [];
    list.push({
      offset: span.offset,
      length: typeof span.length === "number" ? span.length : 0,
      severity: diagnostic.severity === "warning" ? "warning" : "error",
      message: diagnostic.message ?? "",
    });
    byFile.set(span.file, list);
  }
  for (const file of markedFiles) {
    if (!byFile.has(file)) {
      editor.setMarks(file, []);
    }
  }
  markedFiles.clear();
  for (const [file, marks] of byFile) {
    editor.setMarks(file, marks);
    markedFiles.add(file);
  }
}

export function createDiagnosticsSession(options: {
  elements: DiagnosticsElements;
  source: ProjectReader;
  setStatus: StatusReporter;
  onReady: (ready: boolean) => void;
  /**
   * The finished compile's diagnostics array, handed over unchanged after the list is rendered. The Problems
   * panel and the status bar's split counts derive from it; the session's own output is not altered.
   */
  onDiagnostics?: (diagnostics: readonly Diagnostic[]) => void;
}): DiagnosticsSession {
  const { editor, list, emptyState, statusCount } = options.elements;
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
    // The status line's count first: it is the one place the number survives a view switch, and the
    // pane's own status is the sentence a reader of *this* pane wants.
    if (statusCount) {
      statusCount.textContent =
        `${counts.errors} ${counts.errors === 1 ? "error" : "errors"}, ${counts.warnings} ${counts.warnings === 1 ? "warning" : "warnings"}`;
    }
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
      const target = options.source();
      // The marshalling and the two release paths live in `lib/wasm-call.ts`: the three views share the
      // same C ABI shape, so the lifetime rule is written once instead of three times.
      stageProject(module, target);
      const json = callExport(module, "_tessera_diagnostics_json");
      const document_ = JSON.parse(json) as DiagnosticsDocument;
      const counts = renderDiagnostics(list, emptyState, document_, { element: editor, file: target.active });
      markEditor(editor, document_);
      report(counts);
      options.onDiagnostics?.(Array.isArray(document_.diagnostics) ? document_.diagnostics : []);
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
      requireExports(await factory(), ["_tessera_diagnostics_json", "_tessera_project_begin"]);
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
    /**
     * An edit was applied (issue #444). On this view that is exactly a Run: compiling is a pure function
     * of the source, there is no state to carry and nothing to reset, and a source that does not compile
     * is already a *successful* answer here (the diagnostics list is the point). What the live edit
     * changes for this pane is only *when* it happens -- the page's debounce, not a button -- so there is
     * deliberately no second implementation.
     */
    apply: run,
    /**
     * "Load sample" on this view, split across the page (issue #431): `sampleSource` is the text the page
     * writes into the entry file, and `loadSample` is what is left for the view to do afterwards -- nothing
     * here, because the compile happens when the visitor presses Run.
     */
    sampleSource: SAMPLE,
    loadSample: () => {},
    /** A gallery pick: the page has written the file, so only the status line changes. */
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
