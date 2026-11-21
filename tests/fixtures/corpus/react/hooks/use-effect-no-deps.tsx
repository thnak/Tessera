// SPDX-License-Identifier: MIT

// Corpus fixture: a `useEffect` with no dependency list. The rules require the exact list, so this
// is the shape the diagnostic names; it is measured to confirm the rule fires.

import { useEffect } from "react";

export function Logger({ label }: { label: string }) {
  useEffect(() => {
    log(label);
  });
  return (
    <div className="logger">
      <span className="logger-label">{label}</span>
    </div>
  );
}
