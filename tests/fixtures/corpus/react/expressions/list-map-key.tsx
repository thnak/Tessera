// SPDX-License-Identifier: MIT

// Corpus fixture: `.map` with a `key` attribute, the accepted list form. It is the control the
// `filter` and `slice` fixtures are read against.

export function RowList({ rows }: { rows: string[] }) {
  return (
    <ul className="row-list">
      {rows.map((row) => (
        <li key={row} className="row-item">{row}</li>
      ))}
    </ul>
  );
}
