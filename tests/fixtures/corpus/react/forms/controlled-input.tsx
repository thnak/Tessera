// SPDX-License-Identifier: MIT

// Corpus fixture: a controlled input with `value`, `onChange` and `autoFocus`. ADR-066 item 1
// proposes `autoFocus` as a focus request at mount; the rest is already accepted.

import { useState } from "react";

export function SearchField({ placeholder }: { placeholder: string }) {
  const [query, setQuery] = useState<string>("");
  return (
    <div className="search-field">
      <input
        value={query}
        placeholder={placeholder}
        autoFocus
        onChange={(value: string) => setQuery(value)}
      />
    </div>
  );
}
