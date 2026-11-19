// SPDX-License-Identifier: MIT

// The page's side of the workbench (issues #555 and #557): it owns the one registry, registers every live
// command's handler against the page's own callbacks, publishes the context keys, routes chords, and builds
// the menu tree the chrome draws. `src/workbench/` is the data and the pure logic; this file is the only
// place those meet React and the DOM.
//
// **One mount point.** `App` calls `useWorkbench(api)` once with the callbacks it already has and renders
// `<WorkbenchChrome workbench={...} />`; nothing else in the page knows about commands. Handlers read the
// latest `api` through a ref, so the registry is created once and a render never re-registers anything.
//
// The registry, context and handlers are created during the first render (not in an effect) because the
// Node gates render the chrome with `react-dom/server`, which runs no effects: an effect-registered handler
// would leave the SSR chrome with no menus.

import { useCallback, useEffect, useRef, useState, useSyncExternalStore } from "react";
import {
  buildMenuBar,
  buildToolbar,
  createContext,
  createRegistry,
  type CommandArgs,
  type ContextRecord,
  type KeyEventLike,
  type MenuBarMenu,
  type MenuEntry,
  type MenuProviders,
  type MenusDocument,
  type Registry,
  type ChordStep,
  type ToolbarEntry,
  buildContextMenu,
  createNotifications,
  type NotificationInput,
  type Notifications,
} from "./workbench/index.js";
import commandsDocument from "./workbench/commands.json";
import menusDocument from "./workbench/menus.json";
import { EDITOR_COMMANDS, editorFacts } from "./editor/editor-target";
import type { LoadedSample } from "./lib/gallery";
import { DPRS, THEMES, VIEWPORTS } from "./lib/preferences";
import type { ControlValues } from "./regions/StudioChrome";
import type { ProjectIo } from "./page-project-io";
import type { PaneRefs } from "./page-state";
import { usePaletteHost, type PaletteHost } from "./page-palette";
import type { CommandRecord, MenuTarget } from "./workbench/index.js";
import type { Workbench as Dock } from "./dock/use-workbench";
import { layoutContext, layoutHandlers, layoutProviders, useLayoutNotices, type LayoutUndo } from "./page-layout-commands";

const DOCS_URL = "https://github.com/thnak/Tessera/blob/main/docs/README.md";
const ISSUE_URL = "https://github.com/thnak/Tessera/issues/new";

/** Phone breakpoint (`studio-workbench-v1.md` decision 13): 720 CSS px. */
const PHONE_QUERY = "(max-width: 719.98px)";

/** What the page hands the workbench: its own callbacks and the state the context keys read. */
export interface WorkbenchApi {
  /** The dock's layout store and operations: the dock-owned commands (`page-layout-commands.ts`) call into it. */
  dock: Dock;
  io: ProjectIo;
  dirtyCount: number;
  hasProject: boolean;
  controls: ControlValues;
  keepState: boolean;
  setKeepState: (keep: boolean) => void;
  skins: readonly { id: string; name: string }[];
  profiles: readonly { id: string; name: string }[];
  profileId: string;
  components: { names: readonly string[]; chosen: string };
  runEnabled: boolean;
  /** A sample chosen from the welcome view or the Open Sample quick pick becomes the open project. */
  pickSample: (sample: LoadedSample) => void;
  restart: () => void;
  applyNow: () => void;
  share: () => Promise<void>;
  setControl: (name: keyof ControlValues, value: string) => void;
  setCustomSize: (width: number, height: number) => void;
  pickComponent: (name: string) => void;
  /** The project's files, for Quick Open, and the page's way to open one in the editor. */
  files: readonly { path: string }[];
  openFile: (path: string) => void;
  buildId: string;
  /** The share flow's own progress text (`encoding...`, `link copied`), shown beside the toolbar. */
  shareStatus: string;
  refs: Pick<PaneRefs, "pickToggle" | "inspectToggle" | "runInput">;
}

export interface Workbench {
  registry: Registry;
  /** The menu bar as built from the live state (one `Menu` entry at phone width). */
  menus: MenuBarMenu[];
  /** Every top-level menu, never collapsed for a phone: what Alt+letter and the palette read. */
  allMenus: MenuBarMenu[];
  toolbar: ToolbarEntry[];
  phone: boolean;
  /** The notification service: toasts, history, do-not-disturb (issue #559). */
  notifications: Notifications;
  /** Adds a notification; actions run through the registry. Returns its id (or `null` for a malformed call). */
  notify: (input: NotificationInput) => number | null;
  /** Whether the bell's history popover is open. */
  centerOpen: boolean;
  setCenterOpen: (open: boolean) => void;
  /** Help > About (`help.about`): the dialog with the title, description, build and links. */
  aboutOpen: boolean;
  setAboutOpen: (open: boolean) => void;
  buildId: string;
  /** Runs a menu or toolbar entry; an entry whose command needs arguments the entry lacks asks for them. */
  activate: (entry: { command?: string; args?: CommandArgs }) => Promise<void>;
  /** Runs a command id with arguments through the registry and reports a refusal in `status`. */
  run: (id: string, args?: CommandArgs) => Promise<void>;
  /** Marks a modal focus owner (`focus.menu`, `focus.palette`) in the context. */
  setFocusOwner: (owner: "menu" | "palette" | null) => void;
  /** Subscribes the chrome to context and handler changes (a store for `useSyncExternalStore`). */
  subscribe: (listener: () => void) => () => void;
  version: () => number;
  /** The command palette, Quick Open and Go to Line overlay's state and the page facts it needs. */
  palette: PaletteHost;
  /** A context menu (`tree.item`, `editor`, `panel.tab`, `canvas`) as built items; empty when nothing is live. */
  contextItems: (name: string, target?: MenuTarget) => MenuEntry[];
  /** The hidden file input behind `file.openZip`, rendered by the chrome, and what it does with a chosen file. */
  zipInputRef: React.RefObject<HTMLInputElement | null>;
  openZip: (file: File) => void;
  /** The device chip's text, `<skin or profile> · <viewport> · <dpr>x`. */
  chipText: string;
  /** Alt+letter reaches the menu bar through this: the chrome listens and opens the matching menu. */
  onMnemonic: (listener: (letter: string) => boolean) => () => void;
  /** Dirty-file count and project name for the toolbar badge. */
  dirtyCount: number;
}

const COMMANDS = commandsDocument.commands as CommandRecord[];

function platformOf(): "win" | "mac" {
  return typeof navigator !== "undefined" && /Mac|iPhone|iPad/.test(navigator.platform ?? "") ? "mac" : "win";
}

function computeContext(api: WorkbenchApi, extra: ContextRecord): ContextRecord {
  const { controls } = api;
  return {
    "project.open": api.hasProject,
    "project.source": api.io.state.source,
    "project.busy": api.io.state.busy,
    "project.dirtyCount": api.dirtyCount,
    "project.hasRecent": api.io.recents.length > 0,
    "project.folderSupported": api.io.folderSupported,
    "project.multiComponent": api.components.names.length > 1,
    "canvas.available": typeof navigator !== "undefined" && "gpu" in navigator,
    "device.profile": api.profileId === "" ? "custom" : api.profileId,
    "device.skin": controls.skin === "" ? "none" : controls.skin,
    "device.viewport": controls.viewport,
    "device.dpr": controls.dpr,
    "device.fold": controls.foldPosture === "" ? "none" : controls.foldPosture,
    "device.keepState": api.keepState,
    "ui.theme": controls.theme,
    "run.enabled": api.runEnabled,
    "run.component": api.components.chosen,
    ...extra,
  };
}

/** Watches a button's `aria-pressed` and `disabled`, which the sessions own, and reports them as state. */
function useButtonState(ref: { current: HTMLButtonElement | null }): { pressed: boolean; disabled: boolean } {
  const [state, setState] = useState({ pressed: false, disabled: true });
  useEffect(() => {
    const node = ref.current;
    if (node === null) {
      return;
    }
    const read = () => setState({ pressed: node.getAttribute("aria-pressed") === "true", disabled: node.disabled });
    read();
    const observer = new MutationObserver(read);
    observer.observe(node, { attributes: true, attributeFilter: ["aria-pressed", "disabled"] });
    return () => observer.disconnect();
  }, [ref]);
  return state;
}

function usePhone(): boolean {
  return useSyncExternalStore(
    (listener) => {
      const query = globalThis.matchMedia?.(PHONE_QUERY);
      query?.addEventListener("change", listener);
      return () => query?.removeEventListener("change", listener);
    },
    () => globalThis.matchMedia?.(PHONE_QUERY).matches ?? false,
    () => false,
  );
}

function focusTree(): void {
  const tree = document.getElementById("project-tree");
  const target = tree?.querySelector<HTMLElement>('[role="treeitem"][aria-selected="true"]') ?? tree?.querySelector<HTMLElement>('[role="treeitem"]');
  (target ?? tree)?.focus();
}

export function useWorkbench(api: WorkbenchApi): Workbench {
  const apiRef = useRef(api);
  apiRef.current = api;
  const [centerOpen, setCenterOpen] = useState(false);
  const centerRef = useRef(setCenterOpen);
  centerRef.current = setCenterOpen;
  const [aboutOpen, setAboutOpen] = useState(false);
  const setAboutRef = useRef(setAboutOpen);
  setAboutRef.current = setAboutOpen;
  const picker = useButtonState(api.refs.pickToggle);
  const inspector = useButtonState(api.refs.inspectToggle);
  const phone = usePhone();
  // The chooser the Open Zip command clicks; the chrome renders it (a hidden file input) and reports the file.
  const zipInputRef = useRef<HTMLInputElement | null>(null);
  const paletteRef = useRef<PaletteHost | null>(null);
  const providersRef = useRef<MenuProviders>({});
  const undoRef = useRef<LayoutUndo | null>(null);
  const zenHintedRef = useRef(false);

  const hostRef = useRef<{ registry: Registry; notifications: Notifications } | null>(null);
  if (hostRef.current === null) {
    const platform = platformOf();
    const context = createContext(computeContext(api, layoutContext(api.dock, null)));
    const registry = createRegistry(COMMANDS, {
      context,
      platform,
      log: (level, event, detail) => {
        if (level === "error") {
          console.error(event, detail);
        }
      },
    });
    const notifications = createNotifications();
    const publishNotify = () => {
      const view = notifications.snapshot();
      context.set({ "notify.dnd": view.dnd, "notify.unread": view.unread, "notify.any": view.history.length > 0 });
    };
    publishNotify();
    notifications.subscribe(publishNotify);
    const a = () => apiRef.current;
    const handlers: Record<string, (args: CommandArgs) => unknown> = {
      "file.openFolder": () => a().io.openFolder(),
      "file.openZip": () => zipInputRef.current?.click(),
      "file.openRecent": (args) => a().io.reopenRecent(Number(args.index)),
      "file.samples": () => paletteRef.current?.open("", "samples"),
      "file.newFile": () => a().io.newFile(),
      "file.renameFile": () => a().io.renameFile(),
      "file.deleteFile": () => a().io.deleteFile(),
      "file.save": () => a().io.save(),
      "file.download": () => a().io.download(),
      "file.share": () => a().share(),
      "view.palette": () => paletteRef.current?.open(">"),
      "view.quickOpen": () => paletteRef.current?.open("", "files"),
      "view.gotoLine": () => paletteRef.current?.open(":", "line"),
      "help.shortcuts": () => paletteRef.current?.open(">", "shortcuts"),
      "view.explorer": () => focusTree(),
      "view.setTheme": (args) => a().setControl("theme", String(args.value)),
      "view.cycleTheme": () => {
        const order = [...THEMES];
        a().setControl("theme", order[(order.indexOf(a().controls.theme) + 1) % order.length]);
      },
      "run.restart": () => a().restart(),
      "run.applyNow": () => a().applyNow(),
      "run.inputScript": () => a().refs.runInput.current?.click(),
      "run.togglePicker": () => a().refs.pickToggle.current?.click(),
      "run.toggleInspector": () => a().refs.inspectToggle.current?.click(),
      "run.component": (args) => a().pickComponent(String(args.name)),
      "device.profile": (args) => a().setControl("profile", args.id === "custom" ? "" : String(args.id)),
      "device.setSkin": (args) => a().setControl("skin", args.value === "none" ? "" : String(args.value)),
      "device.setViewport": (args) => a().setControl("viewport", String(args.value)),
      "device.setCustomSize": (args) => a().setCustomSize(Number(args.width), Number(args.height)),
      "device.setDpr": (args) => a().setControl("dpr", String(args.value)),
      "device.setFold": (args) => a().setControl("foldPosture", args.value === "none" ? "" : String(args.value)),
      "device.toggleKeepState": () => a().setKeepState(!a().keepState),
      "help.docs": () => void window.open(DOCS_URL, "_blank", "noopener"),
      "help.reportIssue": () => void window.open(ISSUE_URL, "_blank", "noopener"),
      "help.about": () => setAboutRef.current(true),
      "notify.show": (args) => {
        const actions = args.actionLabel && args.actionCommand ? [{ label: String(args.actionLabel), command: String(args.actionCommand) }] : [];
        return { id: notifications.notify({ message: String(args.message), severity: (args.severity as NotificationInput["severity"]) ?? "info", actions, key: args.key === undefined ? undefined : String(args.key) }) };
      },
      "notify.toggleCenter": () => {
        notifications.markRead();
        centerRef.current((open) => !open);
      },
      "notify.clearAll": () => notifications.clearHistory(),
      "notify.toggleDoNotDisturb": () => notifications.setDoNotDisturb(!notifications.snapshot().dnd),
      ...layoutHandlers({ dock: () => apiRef.current.dock, notify: (input) => notifications.notify(input), undo: undoRef, zenHinted: zenHintedRef }),
      ...EDITOR_COMMANDS,
    };
    for (const [id, handler] of Object.entries(handlers)) {
      registry.register(id, handler);
    }
    const problems = registry.integrity();
    if (problems.length > 0) {
      console.error("workbench: command table and handlers disagree", problems);
    }
    hostRef.current = { registry, notifications };
  }
  const { registry, notifications } = hostRef.current;

  // Context keys: the page's state, plus the sessions' own toggles. Published after every render; a re-publish
  // of the same values wakes nobody (`context.set` compares).
  const mnemonicListeners = useRef(new Set<(letter: string) => boolean>());
  const focusOwner = useRef<ContextRecord>({});
  const publish = useCallback(() => {
    registry.context.set(
      computeContext(apiRef.current, {
        "run.picker": picker.pressed,
        "run.inspector": inspector.pressed,
        "layout.phone": phone,
        ...layoutContext(apiRef.current.dock, undoRef.current),
        ...focusOwner.current,
      }),
    );
  }, [registry, picker.pressed, inspector.pressed, phone]);
  useEffect(publish);

  const setFocusOwner = useCallback(
    (owner: "menu" | "palette" | null) => {
      focusOwner.current = { "focus.menu": owner === "menu", "focus.palette": owner === "palette" };
      publish();
    },
    [publish],
  );

  const run = useCallback(
    async (id: string, args?: CommandArgs) => {
      const result = await registry.run(id, args);
      if (!result.ok) {
        notifications.notify({ message: result.message, severity: result.code === "failed" ? "error" : "warning", key: `command:${id}:${result.code}` });
      }
    },
    [registry, notifications],
  );

  // The keyboard: chords resolve against the keymap and the context at the instant of the key. The editor
  // and the tree handle their own keys first and `preventDefault` them; a handled event is skipped, so
  // `Mod+S` in the editor is the editor keymap's one call, not two.
  useEffect(() => {
    let pending: ChordStep[] = [];
    let timer = 0;
    const reset = () => {
      pending = [];
      window.clearTimeout(timer);
    };
    const onKeyDown = (event: KeyboardEvent) => {
      if (event.defaultPrevented || event.isComposing) {
        return;
      }
      const active = document.activeElement;
      const facts = editorFacts();
      const context = {
        ...registry.context.snapshot(),
        "editor.focus": facts.focus,
        "editor.hasSelection": facts.hasSelection,
        "tree.focus": active?.closest?.('[role="tree"]') != null,
      };
      const resolution = registry.keymap().resolve(event as unknown as KeyEventLike, pending, context);
      if (resolution.status === "match") {
        reset();
        event.preventDefault();
        void run(resolution.id);
      } else if (resolution.status === "pending") {
        pending = resolution.pending;
        window.clearTimeout(timer);
        timer = window.setTimeout(reset, 1500);
        event.preventDefault();
      } else if (!("ignored" in resolution && resolution.ignored)) {
        reset();
        // Alt+letter opens a menu (the menu bar's mnemonics) when no command claims the chord.
        if (event.altKey && !event.ctrlKey && !event.metaKey && /^[a-z]$/i.test(event.key)) {
          const code = /^Key([A-Z])$/.exec(event.code)?.[1] ?? event.key;
          for (const listener of mnemonicListeners.current) {
            if (listener(code)) {
              event.preventDefault();
              break;
            }
          }
        }
      }
    };
    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
      window.clearTimeout(timer);
    };
  }, [registry, run]);

  // A test and automation seam, like `__tesseraEditor`: drive a command by id.
  useEffect(() => {
    // Merged into the handle the dock hook already published (`commands`, `dispatch`, `layout`).
    const handle = ((window as unknown as { __tesseraWorkbench?: object }).__tesseraWorkbench ??= {});
    Object.assign(handle, {
      run: (id: string, args?: CommandArgs) => registry.run(id, args),
      describe: (id: string, args?: CommandArgs) => registry.describe(id, args),
      context: () => registry.context.snapshot(),
    });
  }, [registry]);

  // A stored layout set aside at boot ("Layout from a newer studio; using the default") is a notification.
  useLayoutNotices(api.dock.notice, notifications);

  // The project's last action and the share flow say what they did through the notification service, so the
  // person hears it once (a toast and the bell history) instead of from one live region per region.
  const projectNote = api.io.state.note;
  useEffect(() => {
    if (projectNote !== "") {
      notifications.notify({ message: projectNote, key: "project-note" });
    }
  }, [projectNote, notifications]);
  const shareText = api.shareStatus;
  useEffect(() => {
    if (shareText !== "" && !shareText.endsWith("...")) {
      notifications.notify({ message: `Share: ${shareText}`, key: "share" });
    }
  }, [shareText, notifications]);

  const providers: MenuProviders = {
    recent: () =>
      api.io.recents.map((recent, index) => ({
        label: recent.name,
        detail: recent.openedAt > 0 ? new Date(recent.openedAt).toLocaleDateString() : undefined,
        args: { index },
      })),
    components: () => api.components.names.map((name) => ({ label: name, args: { name } })),
    profiles: () => [{ label: "Custom size", args: { id: "custom" } }, ...api.profiles.map((entry) => ({ label: entry.name, args: { id: entry.id } }))],
    skins: () => [{ label: "None", args: { value: "none" } }, ...api.skins.map((skin) => ({ label: skin.name, args: { value: skin.id } }))],
    viewports: () => VIEWPORTS.map(([width, height, label]) => ({ label, args: { value: `${width}x${height}` } })),
    dprs: () => DPRS.map((dpr) => ({ label: `${dpr}×`, args: { value: String(dpr) } })),
  };
  Object.assign(providers, layoutProviders(() => apiRef.current.dock));
  providersRef.current = providers;
  const palette = usePaletteHost(apiRef, providersRef);
  paletteRef.current = palette;
  const deps = { registry, providers };
  const allMenus = buildMenuBar(menusDocument as unknown as MenusDocument, deps);
  const toolbar = buildToolbar(menusDocument as unknown as MenusDocument, deps);
  const menus: MenuBarMenu[] = phone
    ? [
        {
          id: "menu",
          title: "Menu",
          mnemonic: "",
          items: allMenus.map(
            (menu): MenuEntry => ({ kind: "submenu", key: `phone/${menu.id}`, label: menu.title, items: menu.items, chord: "", enabled: true, reason: "", checked: false, description: "" }),
          ),
        },
      ]
    : allMenus;

  const activate = useCallback(
    async (entry: { command?: string; args?: CommandArgs }) => {
      if (entry.command !== undefined) {
        await run(entry.command, entry.args);
      }
    },
    [run],
  );

  const skinName = api.skins.find((skin) => skin.id === api.controls.skin)?.name;
  const profileName = api.profiles.find((profile) => profile.id === api.profileId)?.name;
  const chipText = [skinName ?? profileName ?? "Custom", api.controls.viewport.replace("x", "×"), `${api.controls.dpr}×`].join(" · ");

  return {
    registry,
    menus,
    allMenus,
    toolbar,
    phone,
    notifications,
    notify: (input) => notifications.notify(input),
    centerOpen,
    setCenterOpen,
    aboutOpen,
    setAboutOpen,
    buildId: api.buildId,
    activate,
    run,
    setFocusOwner,
    subscribe: registry.subscribe,
    version: registry.version,
    zipInputRef,
    openZip: (file) => apiRef.current.io.openZip(file),
    palette,
    contextItems: (name, target) => buildContextMenu(menusDocument as unknown as MenusDocument, name, deps, target),
    chipText,
    onMnemonic: (listener) => {
      mnemonicListeners.current.add(listener);
      return () => void mnemonicListeners.current.delete(listener);
    },
    dirtyCount: api.dirtyCount,
  };
}
