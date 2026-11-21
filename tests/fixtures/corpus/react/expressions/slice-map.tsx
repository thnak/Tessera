// SPDX-License-Identifier: MIT

// Corpus fixture: `.slice()` for a "first N" list. ADR-066 item 3 proposes `slice` next to
// `filter`; the fixture keeps the two measured separately.

export function RecentRows({ rows }: { rows: { id: string; label: string }[] }) {
  return (
    <div className="recent">
      {rows.slice(0, 5).map((row) => (
        <span key={row.id} className="recent-row">{row.label}</span>
      ))}
    </div>
  );
}
