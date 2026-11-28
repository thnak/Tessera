// SPDX-License-Identifier: MIT

// Splitter semantics the library does not provide (W0 gaps, studio-workbench-layout-v1.md sections 4.4 and
// 5.6): real `aria-valuemin`/`aria-valuemax`/`aria-valuenow`, Shift+arrow (64 px) and Home/End. Pure
// functions over fractions; the DOM glue is `FlexDock.tsx`. The result of a key is an ordinary `resize`
// operation, so a keyboard resize and a pointer drag end in the same place.

import { canonicalSizes, minFractionsOf, type Layout, type LayoutNode } from "../workbench/layout";
import { visibleLayout } from "./codec";

export const ARROW_STEP_PX = 16;
export const SHIFT_STEP_PX = 64;

export { minFractionsOf };

export interface SeparatorValues {
  /** Percent of the split's extent where the splitter sits. */
  now: number;
  min: number;
  max: number;
}

/** `index` is the splitter between child `index` and child `index + 1`. */
export function separatorValues(sizes: readonly number[], index: number, minFractions: readonly number[]): SeparatorValues {
  const before = sizes.slice(0, index).reduce((sum, size) => sum + size, 0);
  const now = before + sizes[index];
  const min = before + minFractions[index];
  const max = now + sizes[index + 1] - minFractions[index + 1];
  const pct = (fraction: number): number => Math.round(fraction * 100);
  return { now: pct(now), min: pct(min), max: pct(Math.max(max, min)) };
}

/** Moves the splitter by `deltaFraction`, clamped so neither neighbour drops under its minimum. */
export function moveSplitter(
  sizes: readonly number[],
  index: number,
  deltaFraction: number,
  minFractions: readonly number[],
): number[] {
  const pair = sizes[index] + sizes[index + 1];
  const left = Math.min(pair - minFractions[index + 1], Math.max(minFractions[index], sizes[index] + deltaFraction));
  const out = [...sizes];
  out[index] = left;
  out[index + 1] = pair - left;
  return canonicalSizes(out);
}

export type SplitterKey = "ArrowLeft" | "ArrowRight" | "ArrowUp" | "ArrowDown" | "Home" | "End";

/** The new sizes a key asks for, or `null` for a key that is not this splitter's. */
export function sizesForKey(
  key: string,
  shift: boolean,
  vertical: boolean,
  sizes: readonly number[],
  index: number,
  extentPx: number,
  minFractions: readonly number[],
): number[] | null {
  const forward = vertical ? "ArrowDown" : "ArrowRight";
  const back = vertical ? "ArrowUp" : "ArrowLeft";
  if (extentPx <= 0) {
    return null;
  }
  if (key === "Home") {
    return moveSplitter(sizes, index, -1, minFractions);
  }
  if (key === "End") {
    return moveSplitter(sizes, index, 1, minFractions);
  }
  if (key !== forward && key !== back) {
    return null;
  }
  const step = (shift ? SHIFT_STEP_PX : ARROW_STEP_PX) / extentPx;
  return moveSplitter(sizes, index, key === forward ? step : -step, minFractions);
}

/** The split a model path names in the *visible* layout (collapsed groups are not rendered). */
export function splitAtPath(layout: Layout, path: readonly number[]): Extract<LayoutNode, { type: "split" }> | null {
  let node: LayoutNode = visibleLayout(layout).root;
  for (const index of path) {
    if (node.type !== "split" || index >= node.children.length) {
      return null;
    }
    node = node.children[index];
  }
  return node.type === "split" ? node : null;
}

/** `data-layout-path` of a library splitter: the row's path (`/r1/ts0`) plus `/s<index>`. */
export function parseSplitterPath(attribute: string | null): { path: number[]; index: number } | null {
  if (attribute === null) {
    return null;
  }
  const match = /^((?:\/(?:r|ts)\d+)*)\/s(\d+)$/.exec(attribute);
  if (match === null) {
    return null;
  }
  const path = [...match[1].matchAll(/\/(?:r|ts)(\d+)/g)].map((entry) => Number(entry[1]));
  return { path, index: Number(match[2]) };
}
