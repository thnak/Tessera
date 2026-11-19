// SPDX-License-Identifier: MIT

// `view.focusNextRegion` / `view.focusPreviousRegion` (F6 and Shift+F6): the studio's regions in reading order,
// which is DOM order (`studio-workbench-layout-v1.md` section 4.4): the menu bar, the toolbar, each dock group
// left to right and top to bottom, then the status line. One call moves focus to the first focusable thing of
// the next region; a hidden region (zen mode, a collapsed group) is skipped because it has no layout box.

import { panelOfTab } from "./dock/tab-id";

interface Region {
  container: Element;
  /** Whether `element` belongs to the region (a dock group owns its panels' content as well as its tab strip). */
  owns: (element: Element) => boolean;
  target: () => HTMLElement | null;
}

const visible = (element: Element): boolean => element.getClientRects().length > 0;

function firstFocusable(container: Element, selector: string): HTMLElement | null {
  return [...container.querySelectorAll<HTMLElement>(selector)].find((element) => visible(element)) ?? null;
}

function simple(container: Element, selector: string): Region {
  return { container, owns: (element) => container.contains(element), target: () => firstFocusable(container, selector) };
}

/** Every region that can take focus now, in reading order. */
export function regions(): Region[] {
  const out: Region[] = [];
  const bar = document.querySelector('[role="menubar"]');
  if (bar !== null) {
    out.push(simple(bar, '[tabindex="0"], [role="menuitem"]'));
  }
  const toolbar = document.querySelector('[role="toolbar"]');
  if (toolbar !== null) {
    out.push(simple(toolbar, '[tabindex="0"], button'));
  }
  for (const tabset of document.querySelectorAll<HTMLElement>(".dock-host .flexlayout__tabset")) {
    const panels = [...tabset.querySelectorAll('[role="tab"]')].map((tab) => panelOfTab(tab));
    out.push({
      container: tabset,
      owns: (element) => {
        const host = element.closest<HTMLElement>("[data-panel]")?.dataset.panel;
        return tabset.contains(element) || (host !== undefined && panels.includes(host));
      },
      target: () => firstFocusable(tabset, '[role="tab"][aria-selected="true"]') ?? firstFocusable(tabset, '[role="tab"]'),
    });
  }
  const status = document.querySelector(".page-footer");
  if (status !== null) {
    out.push(simple(status, "button, a[href], [tabindex='0']"));
  }
  return out.filter((region) => visible(region.container) && region.target() !== null);
}

/** Moves focus `step` regions on (wrapping) and returns the element focused, or `null` when there is nowhere to go. */
export function focusRegion(step: 1 | -1): HTMLElement | null {
  const list = regions();
  if (list.length === 0) {
    return null;
  }
  const active = document.activeElement;
  const current = list.findIndex((region) => active !== null && region.owns(active));
  const next = current < 0 ? (step === 1 ? 0 : list.length - 1) : (current + step + list.length) % list.length;
  const element = list[next].target();
  element?.focus();
  return element;
}
