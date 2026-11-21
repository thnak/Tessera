// SPDX-License-Identifier: MIT

// Corpus fixture: `.filter(...).map(...)` in JSX. ADR-066 item 3 proposes `filter` and `slice` in
// the pure-expression allowlist; the coverage table calls this the largest gap by frequency.

export function ActiveRows({ rows }: { rows: { id: string; label: string; on: boolean }[] }) {
  return (
    <div className="rows">
      {rows
        .filter((row) => row.on)
        .map((row) => (
          <span key={row.id} className="row">{row.label}</span>
        ))}
    </div>
  );
}
