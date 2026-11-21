// SPDX-License-Identifier: MIT

// Corpus fixture: a hook called after a statement that reads a state declared further down. The
// rules require hooks first, so this is the order violation the diagnostic names.

import { useState } from "react";

export function LateHook() {
  const doubled = count + count;
  const [count, setCount] = useState<int>(0);
  return (
    <div className="late-hook" onClick={() => setCount(doubled)}>
      <span className="late-value">{count}</span>
    </div>
  );
}
