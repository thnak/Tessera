// SPDX-License-Identifier: MIT

// Corpus fixture: `forwardRef` and a callback ref. ADR-066 item 8 keeps both rejected; a component
// exposes a callback prop instead, and `useRef` covers the imperative handle.

import { forwardRef, useRef } from "react";

export const FancyInput = forwardRef<HTMLInputElement, { label: string }>((props, ref) => {
  const local = useRef<Element>(null);
  return (
    <div className="fancy-input" ref={local}>
      <span className="fancy-label">{props.label}</span>
    </div>
  );
});
