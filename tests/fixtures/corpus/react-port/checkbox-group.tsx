// SPDX-License-Identifier: MIT

// Corpus (spike #660): checkbox with checked and onChange

import { useState } from "react";

export function Agree() {
  const [agreed, setAgreed] = useState(false);
  return (
    <label>
      <input type="checkbox" checked={agreed} onChange={(e) => setAgreed(e.target.checked)} />
      I agree
    </label>
  );
}
