// SPDX-License-Identifier: MIT

// The phone-width projection (studio-workbench-layout-v1.md section 3.4): below 720 CSS px the dock shows
// one panel at a time from a flat list. A pure function of the layout; the persisted desktop layout is never
// modified, so a resize round trip rewrites nothing.

import { openPanels } from "./tree";
import type { Layout } from "./types";

/** The viewport width under which the dock collapses to one visible group. */
export const PHONE_BREAKPOINT_PX = 720;

const PHONE_ORDER = ["editor", "canvas", "scene", "diagnostics"] as const;

export interface PhoneView {
  /** Every open panel: Editor, Canvas, Scene, Diagnostics, then the rest in layout reading order. */
  panels: string[];
  active: string;
}

export function collapse(layout: Layout, prefer?: string): PhoneView {
  const open = openPanels(layout);
  const head = PHONE_ORDER.filter((id) => open.includes(id));
  const panels = [...head, ...open.filter((id) => !head.includes(id as (typeof PHONE_ORDER)[number]))];
  const active = prefer !== undefined && panels.includes(prefer) ? prefer : (panels[0] ?? "");
  return { panels, active };
}

export function isPhoneWidth(widthCssPx: number): boolean {
  return widthCssPx < PHONE_BREAKPOINT_PX;
}
