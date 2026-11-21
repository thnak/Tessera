// SPDX-License-Identifier: MIT

// Corpus fixture: a render prop, a function passed as a child. ADR-066 item 8 keeps this rejected:
// function values are not part of the model, so the port uses a `children` slot.

export function List({ render }: { render: () => void }) {
  return (
    <div className="list">
      {render()}
    </div>
  );
}
