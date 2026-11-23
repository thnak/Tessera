// SPDX-License-Identifier: MIT

// Corpus fixture: a `useMemo` dependency list that omits a name the body reads (issue #829).
// Under React this keeps a stale value when `rows` changes; Tessera infers the dependency and
// recomputes, so the list only warns (`TESSERA_REACT_MEMO_DEPS`) and the file is still accepted.

import { useMemo } from "react";

export function FilteredList({ rows }: { rows: string[] }) {
  const count = useMemo(() => rows.length, []);
  return (
    <div className="filtered-list">
      <span className="filtered-count">{count}</span>
    </div>
  );
}
