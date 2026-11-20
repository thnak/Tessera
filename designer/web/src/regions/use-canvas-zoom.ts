// SPDX-License-Identifier: MIT

// The canvas pane's zoom and pan (the arithmetic is `views/canvas-zoom.ts`).
//
// **Chrome state, not engine state.** The zoom lives in React because it is a control the person reads, but
// it only ever writes a CSS transform and a sizer's size: the canvas element is not re-created, the session
// is not told, and the engine is not asked to lay out again. The stage that carries the transform is a
// wrapper that is always rendered, so React never moves the canvas to apply it.
//
// Pan is the frame's own scroll. The page adds the gestures that a scroll bar alone does not give: Ctrl +
// wheel zooms around the cursor, the middle button drags, and the pan tool makes the primary button drag
// (it is off by default because the primary button belongs to the app being previewed).

import { useCallback, useEffect, useLayoutEffect, useRef, useState, type RefObject } from "react";
import {
  anchoredScroll,
  clampZoom,
  fitZoom,
  stepZoom,
  type ZoomMode,
} from "../views/canvas-zoom";

/** The frame's padding, which the stage cannot use. Kept in step with `.canvas-frame` in `preview.css`. */
const FRAME_PADDING = 12;
/** Wheel sensitivity: a notch of 100 changes the zoom by about 16%. */
const WHEEL_RATE = 0.0015;

export interface CanvasZoom {
  stageRef: RefObject<HTMLDivElement | null>;
  mode: ZoomMode;
  zoom: number;
  /** The layout size of the stage times the zoom: what the sizer reserves so the frame can scroll. */
  scaled: { width: number; height: number } | null;
  panTool: boolean;
  setPanTool: (on: boolean) => void;
  zoomIn: () => void;
  zoomOut: () => void;
  actualSize: () => void;
  fit: () => void;
}

export function useCanvasZoom(frameRef: RefObject<HTMLDivElement | null>): CanvasZoom {
  const stageRef = useRef<HTMLDivElement | null>(null);
  const [mode, setMode] = useState<ZoomMode>({ kind: "fit" });
  const [natural, setNatural] = useState({ width: 0, height: 0 });
  const [available, setAvailable] = useState({ width: 0, height: 0 });
  const [panTool, setPanTool] = useState(false);
  // The cursor point a wheel zoom keeps still, applied once the new size has been laid out.
  const anchor = useRef<{ x: number; y: number; before: number } | null>(null);

  const zoom = mode.kind === "fit" ? fitZoom(available, natural) : clampZoom(mode.value);
  const zoomRef = useRef(zoom);
  zoomRef.current = zoom;

  // The stage's layout size (a transform does not change it) and the frame's usable size.
  useEffect(() => {
    const stage = stageRef.current;
    const frame = frameRef.current;
    if (stage === null || frame === null || typeof ResizeObserver === "undefined") {
      return undefined;
    }
    const measureStage = (): void =>
      setNatural({ width: stage.offsetWidth, height: stage.offsetHeight });
    const measureFrame = (): void =>
      setAvailable({
        width: frame.clientWidth - FRAME_PADDING * 2,
        height: frame.clientHeight - FRAME_PADDING * 2,
      });
    measureStage();
    measureFrame();
    const stageObserver = new ResizeObserver(measureStage);
    const frameObserver = new ResizeObserver(measureFrame);
    stageObserver.observe(stage);
    frameObserver.observe(frame);
    return () => {
      stageObserver.disconnect();
      frameObserver.disconnect();
    };
  }, [frameRef]);

  const setScale = useCallback((value: number) => {
    setMode({ kind: "scale", value: clampZoom(value) });
  }, []);

  // A wheel zoom keeps the point under the cursor still: the scroll offset is corrected after the sizer
  // has taken its new size, before the browser paints.
  useLayoutEffect(() => {
    const frame = frameRef.current;
    const pending = anchor.current;
    if (frame === null || pending === null) {
      return;
    }
    anchor.current = null;
    frame.scrollLeft = anchoredScroll(frame.scrollLeft, pending.x, pending.before, zoom);
    frame.scrollTop = anchoredScroll(frame.scrollTop, pending.y, pending.before, zoom);
  }, [zoom, frameRef]);

  // Ctrl + wheel (and a trackpad pinch, which arrives as the same event) zooms; a plain wheel scrolls.
  // The listener is added by hand because React's wheel handler is passive and could not stop the page
  // from zooming itself.
  useEffect(() => {
    const frame = frameRef.current;
    if (frame === null) {
      return undefined;
    }
    const onWheel = (event: WheelEvent): void => {
      if (!event.ctrlKey) {
        return;
      }
      event.preventDefault();
      const box = frame.getBoundingClientRect();
      const before = zoomRef.current;
      const after = clampZoom(before * Math.exp(-event.deltaY * WHEEL_RATE));
      if (after === before) {
        return;
      }
      anchor.current = { x: event.clientX - box.left, y: event.clientY - box.top, before };
      setScale(after);
    };
    frame.addEventListener("wheel", onWheel, { passive: false });
    return () => frame.removeEventListener("wheel", onWheel);
  }, [frameRef, setScale]);

  // Drag to pan: the middle button always, the primary button while the pan tool is on. The listener is in
  // the capture phase so a pan never reaches the canvas as a press.
  useEffect(() => {
    const frame = frameRef.current;
    if (frame === null) {
      return undefined;
    }
    let drag: { id: number; x: number; y: number } | null = null;
    frame.classList.toggle("pan-tool", panTool);
    const onDown = (event: PointerEvent): void => {
      const wanted = event.button === 1 || (event.button === 0 && panTool);
      if (!wanted) {
        return;
      }
      event.preventDefault();
      event.stopPropagation();
      drag = { id: event.pointerId, x: event.clientX, y: event.clientY };
      try {
        frame.setPointerCapture(event.pointerId);
      } catch {
        // a synthetic pointer cannot be captured; the drag still follows the moves it is sent
      }
      frame.classList.add("is-panning");
    };
    const onMove = (event: PointerEvent): void => {
      if (drag === null || event.pointerId !== drag.id) {
        return;
      }
      frame.scrollLeft -= event.clientX - drag.x;
      frame.scrollTop -= event.clientY - drag.y;
      drag = { id: drag.id, x: event.clientX, y: event.clientY };
    };
    const onEnd = (event: PointerEvent): void => {
      if (drag === null || event.pointerId !== drag.id) {
        return;
      }
      drag = null;
      frame.classList.remove("is-panning");
    };
    frame.addEventListener("pointerdown", onDown, true);
    frame.addEventListener("pointermove", onMove);
    frame.addEventListener("pointerup", onEnd);
    frame.addEventListener("pointercancel", onEnd);
    return () => {
      frame.removeEventListener("pointerdown", onDown, true);
      frame.removeEventListener("pointermove", onMove);
      frame.removeEventListener("pointerup", onEnd);
      frame.removeEventListener("pointercancel", onEnd);
      frame.classList.remove("is-panning", "pan-tool");
    };
  }, [frameRef, panTool]);

  const scaled =
    natural.width > 0 && natural.height > 0
      ? { width: natural.width * zoom, height: natural.height * zoom }
      : null;

  return {
    stageRef,
    mode,
    zoom,
    scaled,
    panTool,
    setPanTool,
    zoomIn: () => setScale(stepZoom(zoom, 1)),
    zoomOut: () => setScale(stepZoom(zoom, -1)),
    actualSize: () => setScale(1),
    fit: () => setMode({ kind: "fit" }),
  };
}
