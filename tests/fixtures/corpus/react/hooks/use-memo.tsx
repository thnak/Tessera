// SPDX-License-Identifier: MIT

// Corpus fixture: `useMemo` over a pure expression. ADR-066 item 1 proposes to lower the safe form
// to a local; the memo is a performance hint, and updates already follow dependencies.

import { useMemo, useState } from "react";

export function FilteredList({ rows }: { rows: string[] }) {
  const [query, setQuery] = useState<string>("");
  const visible = useMemo(() => rows.filter((row) => row.startsWith(query)), [rows, query]);
  return (
    <div className="filtered-list">
      <input value={query} onChange={(value: string) => setQuery(value)} />
      {visible.map((row) => (
        <span key={row} className="filtered-row">{row}</span>
      ))}
    </div>
  );
}
