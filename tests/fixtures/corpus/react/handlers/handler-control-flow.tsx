// SPDX-License-Identifier: MIT

// Corpus fixture: `if` and an early `return` inside a handler. ADR-066 item 2 proposes the
// conditional action in the handler IR; this is the body shape it is written for.

import { useState } from "react";

export function SubmitButton({ disabled }: { disabled: boolean }) {
  const [busy, setBusy] = useState<boolean>(false);
  const submit = () => {
    if (disabled) {
      return;
    }
    const next = !busy;
    setBusy(next);
  };
  return (
    <div className="submit" onClick={submit}>
      <span className="submit-label">Submit</span>
    </div>
  );
}
