// SPDX-License-Identifier: MIT

// Restore handles for collapsed groups (layout spec 4.4: "dragging the collapsed edge back restores the previous
// size"). A collapsed group is not rendered, so it has no sash; this draws a thin handle on the edge of the
// visible groups beside it. Dragging it back into them by more than a collapse threshold, or double-clicking it,
// is the `collapse` operation with `collapsed: false`, and the group returns at the size it kept. The handle is a
// pointer affordance only (`aria-hidden`): the keyboard has the same operation as a command (`view.toggleSideBar`,
// `view.togglePanelArea`, `view.togglePanel`), which is why it adds no tab stop.

import { useCallback, useEffect, useRef, useState, type PointerEvent as ReactPointerEvent, type ReactNode, type RefObject } from "react";
import { COLLAPSE_PX, collapsedEdges, findGroup, type CollapsedEdge, type Layout, type Operation, type PanelDef } from "../workbench/layout";

const THICKNESS_PX = 4;

interface Placed {
  edge: CollapsedEdge;
  left: number;
  top: number;
  width: number;
  height: number;
  title: string;
}

function tabsetOf(host: HTMLElement, layout: Layout, groupId: string): HTMLElement | null {
  const panel = findGroup(layout, groupId)?.active;
  const tab = panel === undefined ? null : host.querySelector(`[id="flexlayout-tabbutton-${panel}"]`);
  return tab?.closest<HTMLElement>(".flexlayout__tabset") ?? null;
}

function place(host: HTMLElement, layout: Layout, defs: readonly PanelDef[]): Placed[] {
  const origin = host.getBoundingClientRect();
  return collapsedEdges(layout).flatMap((edge) => {
    const rects = edge.beside
      .map((id) => tabsetOf(host, layout, id)?.getBoundingClientRect())
      .filter((rect): rect is DOMRect => rect !== undefined && rect.width > 0);
    if (rects.length === 0) {
      return [];
    }
    const left = Math.min(...rects.map((rect) => rect.left)) - origin.left;
    const top = Math.min(...rects.map((rect) => rect.top)) - origin.top;
    const right = Math.max(...rects.map((rect) => rect.right)) - origin.left;
    const bottom = Math.max(...rects.map((rect) => rect.bottom)) - origin.top;
    const horizontalEdge = edge.dir === "column";
    const title = defs.find((def) => def.id === findGroup(layout, edge.groupId)?.active)?.title ?? edge.groupId;
    return [
      {
        edge,
        title,
        left: horizontalEdge ? left : edge.edge === "end" ? right - THICKNESS_PX : left,
        top: horizontalEdge ? (edge.edge === "end" ? bottom - THICKNESS_PX : top) : top,
        width: horizontalEdge ? right - left : THICKNESS_PX,
        height: horizontalEdge ? THICKNESS_PX : bottom - top,
      },
    ];
  });
}

export interface CollapsedEdgesProps {
  layout: Layout;
  defs: readonly PanelDef[];
  host: RefObject<HTMLElement | null>;
  /** Changes whenever the library may have laid out again. */
  version: string;
  onOperation: (operation: Operation) => void;
}

export function CollapsedEdges({ layout, defs, host, version, onOperation }: CollapsedEdgesProps): ReactNode {
  const [placed, setPlaced] = useState<Placed[]>([]);
  const last = useRef("");
  const grab = useRef<{ id: string; origin: number } | null>(null);

  const measure = useCallback(() => {
    const element = host.current;
    const next = element === null ? [] : place(element, layout, defs);
    const text = JSON.stringify(next);
    if (text !== last.current) {
      last.current = text;
      setPlaced(next);
    }
  }, [host, layout, defs]);

  // An effect, not a layout effect: the dock's wrapper ref is attached after its children's layout effects ran.
  useEffect(() => {
    measure();
    const element = host.current;
    if (element === null) {
      return;
    }
    let frame = 0;
    const later = (): void => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(measure);
    };
    const resize = new ResizeObserver(later);
    resize.observe(element);
    const mutation = new MutationObserver(later);
    mutation.observe(element, { subtree: true, childList: true, attributes: true, attributeFilter: ["style"] });
    return () => {
      cancelAnimationFrame(frame);
      resize.disconnect();
      mutation.disconnect();
    };
  }, [measure, host, version]);

  const restore = (id: string): void => onOperation({ kind: "collapse", groupId: id, collapsed: false });

  return placed.map(({ edge, left, top, width, height, title }) => {
    const horizontalEdge = edge.dir === "column";
    // Back into the visible groups: up for a bottom edge, left for a right edge, and the reverse at a start edge.
    const inward = (event: { clientX: number; clientY: number }): number =>
      (horizontalEdge ? event.clientY : event.clientX) * (edge.edge === "end" ? -1 : 1);
    return (
      <div
        key={edge.groupId}
        className={`dock-edge dock-edge-${horizontalEdge ? "h" : "v"}`}
        data-collapsed-group={edge.groupId}
        aria-hidden="true"
        title={`Drag back or double-click to show ${title}`}
        style={{ left, top, width, height }}
        onPointerDown={(event: ReactPointerEvent<HTMLDivElement>) => {
          if (event.button === 0) {
            event.currentTarget.setPointerCapture(event.pointerId);
            grab.current = { id: edge.groupId, origin: inward(event) };
          }
        }}
        onPointerUp={(event: ReactPointerEvent<HTMLDivElement>) => {
          const current = grab.current;
          grab.current = null;
          if (current !== null && current.id === edge.groupId && inward(event) - current.origin >= COLLAPSE_PX) {
            restore(edge.groupId);
          }
        }}
        onPointerCancel={() => {
          grab.current = null;
        }}
        onDoubleClick={() => restore(edge.groupId)}
      />
    );
  });
}
