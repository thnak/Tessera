// SPDX-License-Identifier: MIT

// Corpus fixture: a Fragment as the returned root. ADR-066 item 8 keeps this rejected (Q18): an
// implicit box would change layout, so the port wraps it in a `div`.

export function Toolbar({ items }: { items: string[] }) {
  return (
    <>
      <span className="toolbar-title">Tools</span>
      <div className="toolbar-items">
        {items.map((item) => (
          <span key={item} className="toolbar-item">{item}</span>
        ))}
      </div>
    </>
  );
}
