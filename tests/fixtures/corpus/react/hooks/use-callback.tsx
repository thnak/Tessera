// SPDX-License-Identifier: MIT

// Corpus fixture: `useCallback` for a setter-only handler. ADR-066 item 1 proposes to lower the
// safe form to an inline handler; the identity of the function is not observable here.

import { useCallback, useState } from "react";

export function Toggle({ label }: { label: string }) {
  const [on, setOn] = useState<boolean>(false);
  const toggle = useCallback(() => setOn(!on), [on]);
  return (
    <div className="toggle" onClick={toggle}>
      <span className="toggle-label">{label}</span>
    </div>
  );
}
