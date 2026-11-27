// SPDX-License-Identifier: MIT

// Corpus (spike #660): pagination with Math.max and Math.min

import { useState } from "react";

export function Pager({ pages }: { pages: number }) {
  const [page, setPage] = useState(1);
  return (
    <div>
      <button onClick={() => setPage(Math.max(1, page - 1))}>Prev</button>
      <span>
        {page} / {pages}
      </span>
      <button onClick={() => setPage(Math.min(pages, page + 1))}>Next</button>
    </div>
  );
}
