// SPDX-License-Identifier: MIT

// Corpus (spike #660): select element with options

import { useState } from "react";

export function Sizes() {
  const [size, setSize] = useState("m");
  return (
    <select value={size} onChange={(e) => setSize(e.target.value)}>
      <option value="s">Small</option>
      <option value="m">Medium</option>
      <option value="l">Large</option>
    </select>
  );
}
