// SPDX-License-Identifier: MIT

// Corpus fixture: a `useMemo` dependency list that names something the body does not read (issue
// #829). `label` is a prop the component uses elsewhere, but the memo body does not read it, so it
// is dead text in the list: the extra name only warns (`TESSERA_REACT_MEMO_DEPS`) and the file is
// still accepted.

import { useMemo } from "react";

export function Doubled({ value, label }: { value: int; label: string }) {
  const doubled = useMemo(() => value * 2, [value, label]);
  return (
    <div className="doubled">
      <span className="doubled-label">{label}</span>
      <span className="doubled-value">{doubled}</span>
    </div>
  );
}
