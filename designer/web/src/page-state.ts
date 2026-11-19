// SPDX-License-Identifier: MIT

// The page's React state and the refs the three view sessions write through.
//
// Split out of `App.tsx` for two reasons that are both about readability at the size limit. First, the
// refs are a *contract*: each one names a node a session will own, and having them declared once, in one
// list, is what makes "React renders this childless and the session fills it" checkable by eye. Second,
// every DOM write the page does outside React goes through `setStatusText` or one of these refs -- there is
// no second way to reach the document, which is what keeps the React/engine boundary honest
// (ADR-054 decision 3).
//
// The workspace's own state (the project, the tabs, the active file) is in `page-workspace.ts`; this file
// stays the page's *chrome* state, which after issue #431 is the controls, the share status
// and the build id.

import type { CodeEditor } from "./editor/code-editor";
import { useRef, useState } from "react";
import type { ControlValues } from "./regions/StudioChrome";
import { INITIAL_ENVIRONMENT } from "./page-defaults";

/** A status line's colour, as `.status.ok`/`.error`/`.warning` spells it. */
export type StatusKind = "ok" | "error" | "warning" | null;

/** Sets a status line's text and colour; the three panes all phrase their state this way. */
export function setStatusText(node: HTMLElement | null, text: string, kind: StatusKind = null): void {
  if (!node) {
    return;
  }
  node.textContent = text;
  node.className = "status" + (kind ? " " + kind : "");
}

/** Every DOM node a view session writes into. Each is a ref; each is rendered childless by React. */
export interface PaneRefs {  /**
   * The editor and the hint under it (issue #431). Both are in the code column now, but they are still
   * part of this bundle: a diagnostics span reveals itself in the editor and the Scene session appends
   * the live viewport to the hint, so both are written from outside React's render cycle.
   */
  source: React.MutableRefObject<CodeEditor | null>;
  editorHint: React.RefObject<HTMLParagraphElement | null>;
  /** The status bar's caret cell: the editor writes `Ln, Col` and the selection size here (not through React). */
  statusCaret: React.RefObject<HTMLSpanElement | null>;
  shareStatus: React.RefObject<HTMLSpanElement | null>;
  diagStatus: React.RefObject<HTMLDivElement | null>;
  diagList: React.RefObject<HTMLOListElement | null>;
  diagEmpty: React.RefObject<HTMLDivElement | null>;
  sceneStatus: React.RefObject<HTMLDivElement | null>;
  nodes: React.RefObject<HTMLOListElement | null>;
  sceneEmpty: React.RefObject<HTMLDivElement | null>;
  script: React.RefObject<HTMLTextAreaElement | null>;
  inputStatus: React.RefObject<HTMLDivElement | null>;
  cells: React.RefObject<HTMLOListElement | null>;
  runInput: React.RefObject<HTMLButtonElement | null>;
  canvasStatus: React.RefObject<HTMLDivElement | null>;
  canvasUnsupported: React.RefObject<HTMLDivElement | null>;
  canvasUnsupportedBody: React.RefObject<HTMLParagraphElement | null>;
  canvasFrame: React.RefObject<HTMLDivElement | null>;
  canvas: React.RefObject<HTMLCanvasElement | null>;
  canvasFailed: React.RefObject<HTMLDivElement | null>;
  canvasFailedBody: React.RefObject<HTMLParagraphElement | null>;
  canvasStats: React.RefObject<HTMLDListElement | null>;
  statFrame: React.RefObject<HTMLElement | null>;
  statDraws: React.RefObject<HTMLElement | null>;
  statVertices: React.RefObject<HTMLElement | null>;
  statNodes: React.RefObject<HTMLElement | null>;
  statDpr: React.RefObject<HTMLElement | null>;
  /** The font row (issue #387): the painted face's name, or why text is absent. */
  statFont: React.RefObject<HTMLElement | null>;
  /** Issue #444: the state row's `<dt>`, hidden until the running evaluation declares a cell. */
  stateRow: React.RefObject<HTMLElement | null>;
  /** Issue #444: the state row's `<dd>`, which the pane fills with one `name=value` per cell. */
  statState: React.RefObject<HTMLElement | null>;
  /**
   * Issue #171's picker: the overlay the outline and the readout are drawn into, and the toggle that
   * arms it. The overlay is `pointer-events: none` in CSS, so it never intercepts a move or a press.
   */
  pickOverlay: React.RefObject<HTMLDivElement | null>;
  pickToggle: React.RefObject<HTMLButtonElement | null>;
  /**
   * Issue #171's style inspector: the panel beside the canvas, the body the inspector fills, and the
   * toggle that arms it. The panel is a page element, not engine paint, so it costs no frame.
   */
  inspectPanel: React.RefObject<HTMLElement | null>;
  inspectBody: React.RefObject<HTMLDivElement | null>;
  inspectToggle: React.RefObject<HTMLButtonElement | null>;
  /**
   * The status line's mirror cells (issue #431). The status line spans the bottom of the workspace, so
   * the two facts it reports from the engine -- the diagnostics count and the last frame -- are written
   * by the session that owns them, into a second cell, rather than routed back through React state
   * (which would put a compile or a GPU frame inside a render cycle).
   */
  statusDiagnostics: React.RefObject<HTMLSpanElement | null>;
}

/** Creates the ref bundle once. A factory, because every entry has to be a *stable* ref. */
export function usePaneRefs(): PaneRefs {
  return {
    // The editor React renders once as a childless textarea; no session writes its text (the page owns
    // the buffers), but a diagnostic's "show in source" and the Scene hint do reach it.
    source: useRef<CodeEditor | null>(null),
    editorHint: useRef<HTMLParagraphElement>(null),
    statusCaret: useRef<HTMLSpanElement>(null),
    shareStatus: useRef<HTMLSpanElement>(null),
    diagStatus: useRef<HTMLDivElement>(null),
    diagList: useRef<HTMLOListElement>(null),
    diagEmpty: useRef<HTMLDivElement>(null),
    sceneStatus: useRef<HTMLDivElement>(null),
    nodes: useRef<HTMLOListElement>(null),
    sceneEmpty: useRef<HTMLDivElement>(null),
    script: useRef<HTMLTextAreaElement>(null),
    inputStatus: useRef<HTMLDivElement>(null),
    cells: useRef<HTMLOListElement>(null),
    runInput: useRef<HTMLButtonElement>(null),
    canvasStatus: useRef<HTMLDivElement>(null),
    canvasUnsupported: useRef<HTMLDivElement>(null),
    canvasUnsupportedBody: useRef<HTMLParagraphElement>(null),
    canvasFrame: useRef<HTMLDivElement>(null),
    // The engine's one surface. React renders it once, as a childless leaf; the canvas session owns the
    // backing store, the WebGPU context and every frame after that.
    canvas: useRef<HTMLCanvasElement>(null),
    canvasFailed: useRef<HTMLDivElement>(null),
    canvasFailedBody: useRef<HTMLParagraphElement>(null),
    canvasStats: useRef<HTMLDListElement>(null),
    statFrame: useRef<HTMLElement>(null),
    statDraws: useRef<HTMLElement>(null),
    statVertices: useRef<HTMLElement>(null),
    statNodes: useRef<HTMLElement>(null),
    statDpr: useRef<HTMLElement>(null),
    statFont: useRef<HTMLElement>(null),
    // Issue #444: the running evaluation's state cells, and the row that hides when there are none.
    stateRow: useRef<HTMLElement>(null),
    statState: useRef<HTMLElement>(null),
    // Issue #171's picker: the overlay over the canvas, and the header toggle that arms it.
    pickOverlay: useRef<HTMLDivElement>(null),
    pickToggle: useRef<HTMLButtonElement>(null),
    // Issue #171's style inspector: the panel, the body it fills, and the toggle that arms it.
    inspectPanel: useRef<HTMLElement>(null),
    inspectBody: useRef<HTMLDivElement>(null),
    inspectToggle: useRef<HTMLButtonElement>(null),
    statusDiagnostics: useRef<HTMLSpanElement>(null),
  };
}

/** What the chrome needs from the page's own state, in one object. */
export interface PageState {
  controls: ControlValues;
  shareStatus: string;
  linkNote: string | null;
  buildId: string;
  /** Whether the active view's session can be restarted (its module is loaded). */
  runEnabled: boolean;
  /**
   * Whether a live edit is scheduled or in flight (issue #444).
   *
   * The editor's hint says so while it is true: "applying..." is the whole point of the debounce being
   * visible rather than magic, and it is the only feedback a keystroke gets that does not move the
   * preview's own status line (which belongs to the pane, not the page).
   */
  applying: boolean;
}

/** The page's state and its one setter per field, created once. */
export function usePageState(): [PageState, PageStateSetters] {
  const [controls, setControls] = useState<ControlValues>({ ...INITIAL_ENVIRONMENT });
  const [shareStatus, setShareStatus] = useState("");
  const [linkNote, setLinkNote] = useState<string | null>(null);
  const [buildId, setBuildId] = useState("build: loading");
  const [runEnabled, setRunEnabled] = useState(false);
  const [applying, setApplying] = useState(false);
  return [
    { controls, shareStatus, linkNote, buildId, runEnabled, applying },
    {
      setControls,
      setShareStatus,
      setLinkNote,
      setBuildId,
      setRunEnabled,
      setApplying,
    },
  ];
}

/** The setters from `usePageState`, as one object so a callback can take the whole bundle. */
export interface PageStateSetters {
  setControls: React.Dispatch<React.SetStateAction<ControlValues>>;
  setShareStatus: React.Dispatch<React.SetStateAction<string>>;
  setLinkNote: React.Dispatch<React.SetStateAction<string | null>>;
  setBuildId: React.Dispatch<React.SetStateAction<string>>;
  setRunEnabled: React.Dispatch<React.SetStateAction<boolean>>;
  setApplying: React.Dispatch<React.SetStateAction<boolean>>;
}

/** The refs bundle under the name the session hook uses it by; one object, two readable names. */
export type PageRefs = PaneRefs;
