// SPDX-License-Identifier: MIT

// The React side of the workbench: one hook that owns the layout store, persistence, the phone projection,
// document visibility and the panel host runtime. The page calls it once; the dock renders from what it
// returns and the command registry (W1) calls `commands`.

import { useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from "react";
import {
  PHONE_BREAKPOINT_PX,
  browserStorage,
  collapse,
  createCommands,
  createLayoutSaver,
  createLayoutStore,
  createPanelRuntime,
  groupOfPanel,
  loadLayout,
  openPanels,
  phoneLayout,
  visiblePanels,
  type Layout,
  type LayoutStore,
  type Operation,
  type PanelRuntime,
  type WorkbenchCommands,
} from "../workbench/layout";

export interface Workbench {
  /** The canonical (desktop) layout; the phone projection is derived from it. */
  layout: Layout;
  /** What the dock renders: the layout, or its one-group phone projection. */
  rendered: Layout;
  phone: boolean;
  /** The panel the phone switcher shows. */
  phoneActive: string;
  open: ReadonlySet<string>;
  /** Visible by the model (active tab, expanded, not maximized away) and the document is visible. */
  visible: ReadonlySet<string>;
  /** The group the viewer last acted in. */
  activeGroup: string | null;
  /** A status-line notice from loading a stored layout ("Layout from a newer studio..."), or `null`. */
  notice: string | null;
  /** The `aria-live` sentence for the last move. */
  announcement: string;
  announce(message: string): void;
  store: LayoutStore;
  commands: WorkbenchCommands;
  runtime: PanelRuntime;
  /** A gesture from the dock (or a command): apply it, with the phone rules. */
  dispatch(operation: Operation): void;
}

function useDocumentVisible(): boolean {
  const [visible, setVisible] = useState(() => typeof document === "undefined" || document.visibilityState !== "hidden");
  useEffect(() => {
    const onChange = (): void => setVisible(document.visibilityState !== "hidden");
    document.addEventListener("visibilitychange", onChange);
    return () => document.removeEventListener("visibilitychange", onChange);
  }, []);
  return visible;
}

function usePhone(): boolean {
  const query = `(max-width: ${PHONE_BREAKPOINT_PX - 0.02}px)`;
  const [phone, setPhone] = useState(() => typeof matchMedia === "function" && matchMedia(query).matches);
  useEffect(() => {
    if (typeof matchMedia !== "function") {
      return;
    }
    const list = matchMedia(query);
    const onChange = (): void => setPhone(list.matches);
    onChange();
    list.addEventListener("change", onChange);
    return () => list.removeEventListener("change", onChange);
  }, [query]);
  return phone;
}

export function useWorkbench(): Workbench {
  const boot = useRef<ReturnType<typeof loadLayout> | null>(null);
  if (boot.current === null) {
    boot.current = loadLayout(browserStorage());
  }
  const parts = useMemo(() => {
    const store = createLayoutStore(boot.current!.layout);
    return { store, commands: createCommands(store), runtime: createPanelRuntime(), saver: createLayoutSaver(browserStorage()) };
  }, []);
  const { store, commands, runtime, saver } = parts;
  const layout = useSyncExternalStore(
    useCallback((listener) => store.subscribe(listener), [store]),
    store.get,
    store.get,
  );
  const phone = usePhone();
  const documentVisible = useDocumentVisible();
  const [phoneActivePanel, setPhoneActive] = useState("editor");
  const [activeGroup, setActiveGroup] = useState<string | null>(null);
  const [announcement, setAnnouncement] = useState("");

  const phoneActive = collapse(layout, phoneActivePanel).active;
  // Zen mode (`shell.zen`) shows the editor alone without touching `maximized` or `collapsed`: it is a view of the
  // layout, so leaving zen puts every group back as it was. Panels stay mounted, they are just not visible.
  const zen = layout.shell.zen && !phone;
  const shown = useMemo(() => (zen ? { ...layout, maximized: "editor" } : layout), [zen, layout]);
  const rendered = useMemo(() => (phone ? phoneLayout(layout, phoneActive) : shown), [phone, layout, phoneActive, shown]);

  // Persist after the last operation (300 ms) and on pagehide; the first render is the loaded layout itself.
  const first = useRef(true);
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    saver.schedule(layout);
  }, [layout, saver]);
  useEffect(() => {
    const flush = (): void => saver.flush();
    globalThis.addEventListener("pagehide", flush);
    return () => globalThis.removeEventListener("pagehide", flush);
  }, [saver]);

  // The host contract: opens, closes and visibility transitions, reported once each.
  useEffect(() => {
    runtime.update(shown, { documentVisible, phoneActive: phone ? phoneActive : null });
  }, [runtime, shown, documentVisible, phone, phoneActive]);

  // The chords (Ctrl+J, Ctrl+B, Ctrl+K Z, ...) are the command registry's keymap: `page-layout-commands.ts` binds
  // `view.togglePanelArea` and its siblings to the plain functions in `commands`.
  //
  // A read-mostly handle for the browser gates: the layout operations as plain functions, and the layout as
  // data. The command registry merges `run`, `describe` and `context` into the same object.
  useEffect(() => {
    const handle = ((globalThis as { __tesseraWorkbench?: object }).__tesseraWorkbench ??= {});
    Object.assign(handle, {
      commands,
      dispatch: (operation: Operation) => store.dispatch(operation),
      layout: () => store.get(),
    });
  }, [commands, store]);

  const dispatch = useCallback(
    (operation: Operation) => {
      if (phone) {
        if (operation.kind === "activate") {
          setPhoneActive(operation.panelId);
          return;
        }
        if (operation.kind === "move" || operation.kind === "split" || operation.kind === "resize") {
          return;
        }
      }
      store.dispatch(operation);
      if ("panelId" in operation && operation.panelId !== null) {
        const group = groupOfPanel(store.get(), operation.panelId);
        if (group !== null) {
          setActiveGroup(group.id);
        }
      }
    },
    [phone, store],
  );

  const open = useMemo(() => new Set(openPanels(layout)), [layout]);
  const visible = useMemo(() => {
    const visibleNow = phone ? new Set([phoneActive]) : visiblePanels(shown);
    return documentVisible ? visibleNow : new Set<string>();
  }, [shown, phone, phoneActive, documentVisible]);
  const announce = useCallback((message: string) => setAnnouncement(message), []);

  return {
    layout,
    rendered,
    phone,
    phoneActive,
    open,
    visible,
    activeGroup,
    notice: boot.current.notice,
    announcement,
    announce,
    store,
    commands,
    runtime,
    dispatch,
  };
}
