// SPDX-License-Identifier: MIT

// The build's panel definitions, default layout and presets as one `Context` the operations read.

import defaultJson from "./layout.default.json";
import presetsJson from "./layout.presets.json";
import panelsJson from "./panels.json";
import type { Context, Layout, PanelDef } from "./types";

export const PANELS: readonly PanelDef[] = panelsJson as PanelDef[];
export const DEFAULT_LAYOUT: Layout = defaultJson as unknown as Layout;
export const PRESETS: Readonly<Record<string, Layout>> = presetsJson as unknown as Record<string, Layout>;

export const defaultContext: Context = { defs: PANELS, defaults: DEFAULT_LAYOUT, presets: PRESETS };

export function panelDef(id: string, ctx: Context = defaultContext): PanelDef | undefined {
  return ctx.defs.find((entry) => entry.id === id);
}

/** A fresh copy of the default layout (callers mutate nothing shared). */
export function freshDefault(ctx: Context = defaultContext): Layout {
  return JSON.parse(JSON.stringify(ctx.defaults)) as Layout;
}
