// SPDX-License-Identifier: MIT

// Corpus (spike #660): keyboard handler that reads e.key

import { useState } from "react";

export function Shortcut() {
  const [last, setLast] = useState("");
  return (
    <div tabIndex={0} onKeyDown={(e) => setLast(e.key)}>
      Last key: {last}
    </div>
  );
}
