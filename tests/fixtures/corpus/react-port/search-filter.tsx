// SPDX-License-Identifier: MIT

// Corpus (spike #660): search box filtering a list with toLowerCase and includes

import { useState } from "react";

export function SearchList({ names }: { names: string[] }) {
  const [query, setQuery] = useState("");
  const shown = names.filter((n) => n.toLowerCase().includes(query.toLowerCase()));
  return (
    <div>
      <input value={query} onChange={(e) => setQuery(e.target.value)} />
      <p>{shown.length} results</p>
      <ul>
        {shown.map((n) => (
          <li key={n}>{n}</li>
        ))}
      </ul>
    </div>
  );
}
