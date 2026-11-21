// SPDX-License-Identifier: MIT

// Corpus fixture: `useRef` with a `focus()` handler statement. Accepted today; the coverage table
// records that `.current` is not, and that callback refs are rejected.

import { useRef } from "react";

export function FocusButton({ label }: { label: string }) {
  const target = useRef<Element>(null);
  const focus = () => {
    target.focus();
  };
  return (
    <div className="focus-button" ref={target} onClick={focus}>
      <span className="focus-label">{label}</span>
    </div>
  );
}
