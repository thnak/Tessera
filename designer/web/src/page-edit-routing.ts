// SPDX-License-Identifier: MIT

// The page's side of Undo/Redo routing (issue #585, rule D3): it reads the DOM focus, remembers the domain
// the user acted on last, asks the two histories for their facts, and publishes the answer as the `edit.*`
// context keys the command table's clauses read. The decision itself is `lib/edit-routing.ts`.
//
// **Why focus is read fresh.** A chord resolves against the context at the instant of the key, and a menu
// click has already moved focus to the menu, so both ask `menu()` rather than a value published after the
// last render. The store only exists so the chrome re-renders when the answer changes (a focus move, an
// override operation).

import { useEffect, useRef, useSyncExternalStore } from "react";
import {
  menuState,
  runEdit,
  targetFor,
  NO_STACK,
  type EditFocus,
  type EditMenuState,
  type OverrideDomain,
  type OverrideDomainHost,
} from "./lib/edit-routing";
import { redoText, textStack, undoText } from "./editor/editor-commit";
import type { ContextRecord } from "./workbench/index.js";

export interface EditRouting extends OverrideDomainHost {
  /** The Edit menu's state at the focus right now. */
  menu(): EditMenuState;
  /** The `edit.*` context keys at the focus right now. */
  context(): ContextRecord;
  /** `edit.undo` / `edit.redo`: acts on the target at the focus right now. */
  run(kind: "undo" | "redo"): boolean;
  /** Records that the user acted in `domain` (focus entered it, or a command ran there). */
  noteActed(domain: "override" | "text"): void;
  /** Tells subscribers the focus moved without changing the acted-on domain (the menu took focus). */
  refresh(): void;
  subscribe(listener: () => void): () => void;
  version(): number;
}

const TEXT_INPUT_TYPES = new Set(["text", "search", "number", "email", "url", "tel", "password", ""]);

/** The kind of place `active` is, for routing. */
export function classifyFocus(active: Element | null): EditFocus {
  if (active === null) {
    return "other";
  }
  // Any text field keeps the browser's own undo until it blurs: the inspector's value fields (a value is
  // one override operation only on Enter or blur), a rename or palette field, and the editor's own search
  // box. This is checked before the editor, whose content element is itself contenteditable.
  if (
    active.tagName === "TEXTAREA" ||
    (active.tagName === "INPUT" && TEXT_INPUT_TYPES.has((active as HTMLInputElement).type))
  ) {
    return "text-input";
  }
  if (active.closest(".cm-editor") !== null) {
    return "editor";
  }
  if (active.id === "canvas") {
    return "canvas";
  }
  if ((active as HTMLElement).isContentEditable) {
    return "text-input";
  }
  return active.closest("#inspect-panel") !== null ? "inspector" : "other";
}

export function createEditRouting(): EditRouting {
  let override: OverrideDomain | null = null;
  let dropOverride: (() => void) | null = null;
  let last: "override" | "text" | null = null;
  let version = 0;
  const listeners = new Set<() => void>();

  const bump = (): void => {
    version += 1;
    for (const listener of [...listeners]) {
      listener();
    }
  };
  const focus = (): EditFocus => classifyFocus(typeof document === "undefined" ? null : document.activeElement);
  const stacks = () => ({ override: override?.state() ?? NO_STACK, text: textStack() });
  const menu = (): EditMenuState => menuState(targetFor(focus(), last), stacks());

  return {
    setOverrideDomain(domain) {
      dropOverride?.();
      override = domain;
      dropOverride = domain === null ? null : domain.subscribe(bump);
      bump();
    },
    menu,
    context() {
      const state = menu();
      return {
        "edit.target": state.target,
        "edit.canUndo": state.canUndo,
        "edit.canRedo": state.canRedo,
        "edit.undoTitle": state.undoTitle,
        "edit.redoTitle": state.redoTitle,
      };
    },
    run(kind) {
      const domains = {
        override: {
          ...(override?.state() ?? NO_STACK),
          undo: () => override?.undo() ?? false,
          redo: () => override?.redo() ?? false,
        },
        text: { ...textStack(), undo: undoText, redo: redoText },
      };
      const { target, changed } = runEdit(kind, focus(), last, domains);
      if (target !== "native") {
        last = target;
      }
      bump();
      return changed;
    },
    noteActed(domain) {
      last = domain;
      bump();
    },
    refresh: bump,
    subscribe(listener) {
      listeners.add(listener);
      return () => listeners.delete(listener);
    },
    version: () => version,
  };
}

/** Creates the routing once, follows focus for the "acted on last" domain, and re-renders on change. */
export function useEditRouting(): EditRouting {
  const ref = useRef<EditRouting | null>(null);
  if (ref.current === null) {
    ref.current = createEditRouting();
  }
  const routing = ref.current;
  useEffect(() => {
    const onFocusIn = (event: FocusEvent): void => {
      const target = event.target instanceof Element ? event.target : null;
      const focus = classifyFocus(target);
      if (focus === "editor") {
        routing.noteActed("text");
      } else if (focus === "canvas" || focus === "inspector" || target?.closest("#inspect-panel") != null) {
        routing.noteActed("override");
      } else {
        // The focus moved to chrome or to a field elsewhere: the answer changes, the acted-on domain does not.
        routing.refresh();
      }
    };
    document.addEventListener("focusin", onFocusIn);
    return () => document.removeEventListener("focusin", onFocusIn);
  }, [routing]);
  useSyncExternalStore(routing.subscribe, routing.version, routing.version);
  return routing;
}
