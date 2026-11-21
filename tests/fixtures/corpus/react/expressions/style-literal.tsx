// SPDX-License-Identifier: MIT

// Corpus fixture: an inline `style` object literal. Accepted today; a computed or spread style is
// the rejected form the coverage table records.

export function Spacer({ height }: { height: int }) {
  return (
    <div className="spacer" style={{ height: height, width: 1 }} />
  );
}
