// SPDX-License-Identifier: MIT

// Corpus fixture: a browser API read in a component. ADR-066 item 8 keeps this rejected: there is
// no `window` or `document` in this model, and this is by design.

export function ViewportWidth() {
  const width = window.innerWidth;
  return (
    <div className="viewport">
      <span className="viewport-width">{width}</span>
    </div>
  );
}
