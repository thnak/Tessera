// SPDX-License-Identifier: MIT

// The page's React state and the refs the three view sessions write through.
//
// Split out of `App.tsx` for two reasons that are both about readability at the size limit. First, the
// refs are a *contract*: each one names a node a session will own, and having them declared once, in one
// list, is what makes "React renders this childless and the session fills it" checkable by eye. Second,
// every DOM write the page does outside React goes through `setStatusText` or one of these refs -- there is
// no second way to reach the document, which is what keeps the React/engine boundary honest
// (ADR-054 decision 3).

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
export interface PaneRefs {
  source: React.RefObject<HTMLTextAreaElement | null>;
  editorHint: React.RefObject<HTMLParagraphElement | null>;
  buildId: React.RefObject<HTMLDivElement | null>;
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
}

/** Creates the ref bundle once. A factory, because every entry has to be a *stable* ref. */
export function usePaneRefs(): PaneRefs {
  return {
    source: useRef<HTMLTextAreaElement>(null),
    editorHint: useRef<HTMLParagraphElement>(null),
    buildId: useRef<HTMLDivElement>(null),
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
  };
}

/** What the four regions need from the page's own state, in one object. */
export interface PageState {
  controls: ControlValues;
  galleryOpen: boolean;
  shareStatus: string;
  linkNote: string | null;
  buildId: string;
  buildIdTitle: string | undefined;
  runEnabled: boolean;
}

/** The page's state and its one setter per field, created once. */
export function usePageState(): [PageState, PageStateSetters] {
  const [controls, setControls] = useState<ControlValues>({ ...INITIAL_ENVIRONMENT });
  const [galleryOpen, setGalleryOpen] = useState(false);
  const [shareStatus, setShareStatus] = useState("");
  const [linkNote, setLinkNote] = useState<string | null>(null);
  const [buildId, setBuildId] = useState("build: loading");
  const [buildIdTitle, setBuildIdTitle] = useState<string | undefined>(
    "The build this page came from",
  );
  const [runEnabled, setRunEnabled] = useState(false);
  return [
    { controls, galleryOpen, shareStatus, linkNote, buildId, buildIdTitle, runEnabled },
    {
      setControls,
      setGalleryOpen,
      setShareStatus,
      setLinkNote,
      setBuildId,
      setBuildIdTitle,
      setRunEnabled,
    },
  ];
}

/** The setters from `usePageState`, as one object so a callback can take the whole bundle. */
export interface PageStateSetters {
  setControls: React.Dispatch<React.SetStateAction<ControlValues>>;
  setGalleryOpen: React.Dispatch<React.SetStateAction<boolean>>;
  setShareStatus: React.Dispatch<React.SetStateAction<string>>;
  setLinkNote: React.Dispatch<React.SetStateAction<string | null>>;
  setBuildId: React.Dispatch<React.SetStateAction<string>>;
  setBuildIdTitle: React.Dispatch<React.SetStateAction<string | undefined>>;
  setRunEnabled: React.Dispatch<React.SetStateAction<boolean>>;
}
