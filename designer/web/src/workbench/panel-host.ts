// SPDX-License-Identifier: MIT

// The panel host contract (studio-workbench-layout-v1.md section 4.2) and the runtime that drives it.
// Framework-agnostic: a panel is anything that can say what to do when it is mounted, shown, resized and
// closed. The runtime reads the *layout model*, never a dock library, so visibility is the same answer
// whichever adapter renders the tabs.
//
//   mount(container)        exactly once per open
//   onVisibility(visible)   active tab && group expanded && not maximized-away && not phone-hidden &&
//                           document visible
//   onResize(w, h)          CSS px
//   onClose()               after this, mount() may be called again by a later open

import { openPanels, visiblePanels } from "./tree";
import type { Layout } from "./types";

export interface Disposable {
  dispose(): void;
}

export interface PanelHost {
  mount(container: HTMLElement): Disposable | void;
  onVisibility(visible: boolean): void;
  onResize?(widthCssPx: number, heightCssPx: number): void;
  onClose(): void;
}

export interface RuntimeInputs {
  /** `document.visibilityState === "visible"`. */
  documentVisible: boolean;
  /** On a phone, the one panel shown; `null` on desktop. */
  phoneActive: string | null;
}

export interface PanelRuntime {
  register(id: string, host: PanelHost): void;
  /** Reconciles panel state with `layout`: opens, closes and visibility transitions, each reported once. */
  update(layout: Layout, inputs: RuntimeInputs): void;
  isOpen(id: string): boolean;
  isVisible(id: string): boolean;
}

export function createPanelRuntime(): PanelRuntime {
  const hosts = new Map<string, PanelHost>();
  const open = new Set<string>();
  const visible = new Set<string>();

  return {
    register(id, host) {
      hosts.set(id, host);
    },
    update(layout, inputs) {
      const nowOpen = new Set(openPanels(layout));
      for (const id of [...open]) {
        if (!nowOpen.has(id)) {
          if (visible.delete(id)) {
            hosts.get(id)?.onVisibility(false);
          }
          open.delete(id);
          hosts.get(id)?.onClose();
        }
      }
      for (const id of nowOpen) {
        open.add(id);
      }
      let shown = visiblePanels(layout);
      if (inputs.phoneActive !== null) {
        shown = new Set([...nowOpen].filter((id) => id === inputs.phoneActive));
      }
      if (!inputs.documentVisible) {
        shown = new Set();
      }
      for (const id of [...visible]) {
        if (!shown.has(id)) {
          visible.delete(id);
          hosts.get(id)?.onVisibility(false);
        }
      }
      for (const id of shown) {
        if (!visible.has(id)) {
          visible.add(id);
          hosts.get(id)?.onVisibility(true);
        }
      }
    },
    isOpen: (id) => open.has(id),
    isVisible: (id) => visible.has(id),
  };
}
