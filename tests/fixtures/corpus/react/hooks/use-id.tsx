// SPDX-License-Identifier: MIT

// Corpus fixture: `useId` for an `aria-labelledby` association. ADR-066 item 1 proposes a stable id
// from the instance key and the call ordinal.

import { useId } from "react";

export function Panel({ title }: { title: string }) {
  const id = useId();
  return (
    <section className="panel" aria-labelledby={id}>
      <h2 className="panel-title" id={id}>{title}</h2>
    </section>
  );
}
