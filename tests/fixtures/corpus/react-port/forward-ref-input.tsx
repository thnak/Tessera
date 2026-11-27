// SPDX-License-Identifier: MIT

// Corpus (spike #660): forwardRef wrapper around an input

import { forwardRef } from "react";

export const TextField = forwardRef<HTMLInputElement, { label: string }>(function TextField(
  { label },
  ref,
) {
  return (
    <label>
      {label}
      <input ref={ref} />
    </label>
  );
});
