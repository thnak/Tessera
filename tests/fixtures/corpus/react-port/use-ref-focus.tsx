// SPDX-License-Identifier: MIT

// Corpus (spike #660): useRef to focus an input

import { useRef } from "react";

export function FocusField() {
  const inputRef = useRef<HTMLInputElement>(null);
  return (
    <div>
      <input ref={inputRef} type="text" />
      <button onClick={() => inputRef.current?.focus()}>Focus</button>
    </div>
  );
}
