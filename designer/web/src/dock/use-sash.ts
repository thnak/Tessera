// SPDX-License-Identifier: MIT

// The pointer half of the sash behaviors (studio-workbench-layout-v1.md section 4.4; the model half is
// `workbench/sash.ts`). The dock library clamps a drag at a group's minimum and reports weights, which cannot
// say "the pointer went 40 px past the minimum" or "the pointer landed 6 px from the default". This hook
// records where the pointer actually put the sash and turns the library's end-of-drag action into one `sash`
// operation, so snap and collapse are decided by the model and persisted in the layout JSON. A double-click
// on a sash is one `resetSplit` operation. Keys never come through here: they are `resize` operations.

import { Actions, type Action } from "flexlayout-react";
import { useCallback, useEffect, useRef, type RefObject } from "react";
import type { Layout, Operation } from "../workbench/layout";
import { pathOfRowId } from "./codec";
import { parseSplitterPath, splitAtPath } from "./separators";

/** A drag shorter than this (CSS px) is a click: it never snaps or collapses. */
const CLICK_PX = 3;

interface Drag {
  path: number[];
  index: number;
  extentPx: number;
  /** The split's sizes when the drag began, and the splitter's place among them. */
  origin: number[];
  start: number;
  from: number;
  at: number;
  vertical: boolean;
}

interface Sash {
  origin: number[];
  path: number[];
  index: number;
  position: number;
  extentPx: number;
}

export interface SashGestures {
  /** Takes the library's end-of-drag action when a sash drag is the cause; `true` means it was handled. */
  intercept(action: Action): boolean;
  /** Double-click on a sash resets that split; `true` means the event was a sash's. */
  doubleClick(target: Element): boolean;
}

const sameList = (a: readonly number[], b: readonly number[]): boolean => a.length === b.length && a.every((n, i) => n === b[i]);

export function useSashGestures(
  wrapper: RefObject<HTMLElement | null>,
  layoutRef: RefObject<Layout>,
  onOperation: (operation: Operation) => void,
): SashGestures {
  const drag = useRef<Drag | null>(null);
  const pending = useRef<Sash | null>(null);
  const emit = useRef(onOperation);
  emit.current = onOperation;

  const flush = useCallback(() => {
    const sash = pending.current;
    pending.current = null;
    if (sash !== null) {
      emit.current({ kind: "sash", ...sash });
    }
  }, []);

  useEffect(() => {
    const root = wrapper.current;
    if (root === null) {
      return;
    }
    const down = (event: PointerEvent): void => {
      const separator = (event.target as Element).closest("[role=separator]");
      const where = separator === null ? null : parseSplitterPath(separator.getAttribute("data-layout-path"));
      const split = where === null ? null : splitAtPath(layoutRef.current, where.path);
      const row = separator?.parentElement ?? null;
      if (event.button !== 0 || where === null || split === null || row === null || where.index + 1 >= split.children.length) {
        return;
      }
      const vertical = split.dir === "column";
      const rect = row.getBoundingClientRect();
      const from = vertical ? event.clientY : event.clientX;
      drag.current = {
        path: where.path,
        index: where.index,
        extentPx: vertical ? rect.height : rect.width,
        origin: [...split.sizes],
        start: split.sizes.slice(0, where.index + 1).reduce((sum, size) => sum + size, 0),
        from,
        at: from,
        vertical,
      };
    };
    const move = (event: PointerEvent): void => {
      const current = drag.current;
      if (current !== null) {
        current.at = current.vertical ? event.clientY : event.clientX;
      }
    };
    const up = (): void => {
      const current = drag.current;
      drag.current = null;
      if (current === null || current.extentPx <= 0 || Math.abs(current.at - current.from) < CLICK_PX) {
        return;
      }
      pending.current = {
        origin: current.origin,
        path: current.path,
        index: current.index,
        position: current.start + (current.at - current.from) / current.extentPx,
        extentPx: current.extentPx,
      };
      // The library reports the drag from its own pointerup; if it has nothing to report (the sash was already
      // at its limit) the overshoot still has to be heard.
      setTimeout(flush, 0);
    };
    const cancel = (): void => {
      drag.current = null;
    };
    root.addEventListener("pointerdown", down, true);
    window.addEventListener("pointermove", move, true);
    window.addEventListener("pointerup", up, true);
    window.addEventListener("pointercancel", cancel, true);
    return () => {
      root.removeEventListener("pointerdown", down, true);
      window.removeEventListener("pointermove", move, true);
      window.removeEventListener("pointerup", up, true);
      window.removeEventListener("pointercancel", cancel, true);
    };
  }, [wrapper, layoutRef, flush]);

  return {
    intercept(action) {
      const sash = pending.current;
      if (sash === null || action.type !== Actions.ADJUST_WEIGHTS) {
        return false;
      }
      const path = pathOfRowId(String((action.data as { nodeId?: unknown }).nodeId));
      if (path === null || !sameList(path, sash.path)) {
        return false;
      }
      flush();
      return true;
    },
    doubleClick(target) {
      const separator = target.closest("[role=separator]");
      const where = separator === null ? null : parseSplitterPath(separator.getAttribute("data-layout-path"));
      if (where === null) {
        return false;
      }
      emit.current({ kind: "resetSplit", path: where.path });
      return true;
    },
  };
}
