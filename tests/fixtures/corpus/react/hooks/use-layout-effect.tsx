// SPDX-License-Identifier: MIT

// Corpus fixture: `useLayoutEffect` with a body and an exact dependency list. ADR-066 item 1
// proposes to accept it with `useEffect` timing, because there is no browser paint to order
// against. The body is deliberately plain so the only rejection is the hook itself.

import { useLayoutEffect } from "react";

export function MeasuredBox({ label }: { label: string }) {
  useLayoutEffect(() => {
    log(label);
  }, [label]);
  return (
    <div className="measured-box">
      <span className="measured-label">{label}</span>
    </div>
  );
}
