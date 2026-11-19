// SPDX-License-Identifier: MIT

// The flexlayout-react adapter (ADR-062; studio-workbench-layout-v1.md section 5). It renders a `Layout`
// (Tessera's model) and reports what the viewer did as an `Operation`; it never owns the layout. The library's
// model is rebuilt whenever the canonical layout changes, and the library's own change for a gesture is
// cancelled in `onAction` (the model changed instead), so library JSON is derived and never persisted.
//
// Three W0 gaps are closed here: real separator `aria-valuemin/max/now`, splitter Shift+arrow and Home/End,
// and keyboard moves of panels (Alt+Shift+Arrow). Delete closes the focused tab; double-clicking a tab
// strip maximizes or restores its group.

import { Layout, Model } from "flexlayout-react";
import { useCallback, useEffect, useLayoutEffect, useMemo, useRef, type KeyboardEvent, type ReactNode } from "react";
import {
  directionTarget,
  findGroup,
  groupOfPanel,
  moveMessage,
  type Direction,
  type Layout as LayoutModel,
  type Operation,
  type PanelDef,
} from "../workbench";
import { gestureOf } from "./actions";
import { toLibrary } from "./codec";
import { PanelSlot } from "./PanelSlot";
import { minFractionsOf, parseSplitterPath, separatorValues, sizesForKey, splitAtPath } from "./separators";

export interface FlexDockProps {
  layout: LayoutModel;
  defs: readonly PanelDef[];
  phone: boolean;
  /** The group the viewer last acted in (the active tabset accent). */
  activeGroup: string | null;
  onOperation: (operation: Operation) => void;
  /** An `aria-live` sentence ("Canvas moved to the bottom"). */
  announce: (message: string) => void;
}

const ARROWS: Record<string, Direction> = { ArrowLeft: "left", ArrowRight: "right", ArrowUp: "top", ArrowDown: "bottom" };

function panelOfTab(element: Element | null): string | null {
  const tab = element?.closest("[role=tab]");
  const id = tab?.getAttribute("id") ?? "";
  const match = /flexlayout-tabbutton-(.+)$/.exec(id) ?? /tabbutton[-_](.+)$/.exec(id);
  return match === null ? (tab?.getAttribute("data-panel-tab") ?? null) : match[1];
}

export function FlexDock({ layout, defs, phone, activeGroup, onOperation, announce }: FlexDockProps): ReactNode {
  const json = useMemo(() => toLibrary(layout, { defs, activeGroup, phone }), [layout, defs, activeGroup, phone]);
  const key = JSON.stringify(json);
  // The library model is rebuilt whenever the canonical layout changes, handing the previous model to
  // `fromJson` so each tab keeps its content element: that is what makes a move, a tab switch or a reset not
  // re-create (or re-parent) a panel's DOM. Held in a ref, not `useMemo`, so React's double render in
  // development cannot adopt the previous model twice.
  const cached = useRef<{ key: string; model: Model } | null>(null);
  if (cached.current === null || cached.current.key !== key) {
    cached.current = { key, model: Model.fromJson(json, cached.current?.model) };
  }
  const model = cached.current.model;
  const layoutRef = useRef(layout);
  layoutRef.current = layout;
  const wrapper = useRef<HTMLDivElement>(null);
  const focusAfter = useRef<string | null>(null);

  const factory = useCallback((node: { getId(): string }) => <PanelSlot panelId={node.getId()} />, []);

  const onAction = useCallback(
    (action: Parameters<NonNullable<React.ComponentProps<typeof Layout>["onAction"]>>[0]) => {
      const gesture = gestureOf(action, layoutRef.current, (groupId) => findGroup(layoutRef.current, groupId)?.active ?? null);
      if (gesture.kind === "pass") {
        return action;
      }
      if (gesture.kind === "operation") {
        if (phone && gesture.operation.kind !== "activate" && gesture.operation.kind !== "close") {
          return undefined;
        }
        onOperation(gesture.operation);
      }
      return undefined;
    },
    [onOperation, phone],
  );

  // ARIA for the splitters: the library reports 0..100 for every separator; the model knows the limits.
  const patchSeparators = useCallback(() => {
    const root = wrapper.current;
    if (root === null) {
      return;
    }
    for (const element of root.querySelectorAll<HTMLElement>("[role=separator]")) {
      const where = parseSplitterPath(element.getAttribute("data-layout-path"));
      const split = where === null ? null : splitAtPath(layoutRef.current, where.path);
      const row = element.parentElement;
      if (where === null || split === null || row === null || where.index + 1 >= split.children.length) {
        continue;
      }
      const rect = row.getBoundingClientRect();
      const extent = split.dir === "row" ? rect.width : rect.height;
      const values = separatorValues(split.sizes, where.index, minFractionsOf(split, extent, defs));
      const wanted: Record<string, string> = {
        "aria-valuenow": String(values.now),
        "aria-valuemin": String(values.min),
        "aria-valuemax": String(values.max),
        "aria-valuetext": `${values.now}%`,
      };
      for (const [name, value] of Object.entries(wanted)) {
        if (element.getAttribute(name) !== value) {
          element.setAttribute(name, value);
        }
      }
    }
  }, [defs]);

  useLayoutEffect(() => {
    patchSeparators();
    const root = wrapper.current;
    if (root === null) {
      return;
    }
    const observer = new MutationObserver(patchSeparators);
    observer.observe(root, {
      subtree: true,
      childList: true,
      attributes: true,
      attributeFilter: ["aria-valuenow", "aria-valuemin", "aria-valuemax"],
    });
    return () => observer.disconnect();
  }, [patchSeparators, key]);

  // Focus returns to a moved panel's tab (spec 4.4).
  useEffect(() => {
    const id = focusAfter.current;
    if (id !== null) {
      focusAfter.current = null;
      requestAnimationFrame(() => {
        const root = wrapper.current;
        const tab = [...(root?.querySelectorAll<HTMLElement>("[role=tab]") ?? [])].find((el) => panelOfTab(el) === id);
        tab?.focus();
      });
    }
  });

  const onKeyDownCapture = (event: KeyboardEvent<HTMLDivElement>): void => {
    const target = event.target as Element;
    const separator = target.closest("[role=separator]");
    if (separator !== null && !event.altKey && !event.ctrlKey && !event.metaKey) {
      const where = parseSplitterPath(separator.getAttribute("data-layout-path"));
      const split = where === null ? null : splitAtPath(layoutRef.current, where.path);
      const row = separator.parentElement;
      if (where !== null && split !== null && row !== null && where.index + 1 < split.children.length) {
        const rect = row.getBoundingClientRect();
        const extent = split.dir === "row" ? rect.width : rect.height;
        const sizes = sizesForKey(event.key, event.shiftKey, split.dir === "column", split.sizes, where.index, extent, minFractionsOf(split, extent, defs));
        if (sizes !== null) {
          event.preventDefault();
          event.stopPropagation();
          onOperation({ kind: "resize", path: where.path, sizes, visible: true });
        }
      }
      return;
    }
    const panelId = panelOfTab(target);
    if (panelId === null) {
      return;
    }
    if (event.altKey && event.shiftKey && ARROWS[event.key] !== undefined) {
      event.preventDefault();
      event.stopPropagation();
      if (phone) {
        return;
      }
      const direction = ARROWS[event.key];
      const move = directionTarget(layoutRef.current, panelId, direction);
      const title = defs.find((def) => def.id === panelId)?.title ?? panelId;
      if (move !== null) {
        focusAfter.current = panelId;
        onOperation({ kind: "move", panelId, target: move });
        announce(moveMessage(title, direction));
      } else {
        announce(`${title} is already at the ${direction}`);
      }
      return;
    }
    if (event.key === "Delete" && !event.altKey && !event.ctrlKey) {
      const closable = defs.find((def) => def.id === panelId)?.closable !== false;
      if (closable && groupOfPanel(layoutRef.current, panelId) !== null) {
        event.preventDefault();
        onOperation({ kind: "close", panelId });
      }
    }
  };

  const onDoubleClick = (event: React.MouseEvent<HTMLDivElement>): void => {
    if (phone) {
      return;
    }
    const panelId = panelOfTab(event.target as Element);
    if (panelId !== null) {
      const maximized = layoutRef.current.maximized;
      onOperation({ kind: "maximize", panelId: maximized === panelId ? null : panelId });
    }
  };

  return (
    <div ref={wrapper} className="dock-host" onKeyDownCapture={onKeyDownCapture} onDoubleClick={onDoubleClick}>
      <Layout
        model={model}
        factory={factory}
        onAction={onAction}
        supportsPopout={false}
        keyMap={{ closeTab: "Alt+W" }}
      />
    </div>
  );
}
