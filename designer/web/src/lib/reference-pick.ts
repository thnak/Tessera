// SPDX-License-Identifier: MIT

// The references of the last Find All References, for the quick pick that lists them (#984). A one-slot store:
// the page sets it, opens the palette on the `references` intent and the overlay reads and chooses from it.

import type { ReferenceRow } from "./analysis-nav";

let current: readonly ReferenceRow[] = [];
let onChoose: (row: ReferenceRow) => void = () => {};

export const referencePick = {
  set(rows: readonly ReferenceRow[], choose: (row: ReferenceRow) => void): void {
    current = rows;
    onChoose = choose;
  },
  rows: (): readonly ReferenceRow[] => current,
  choose(index: number): void {
    const row = current[index];
    if (row !== undefined) {
      onChoose(row);
    }
  },
};
