// SPDX-License-Identifier: MIT

// Corpus fixture: a `let` and a `for` loop inside a handler. ADR-066 item 2 keeps these in native
// handlers; the fixture measures how often the port would need the escape hatch.

import { useState } from "react";

export function SumButton({ values }: { values: int[] }) {
  const [total, setTotal] = useState<int>(0);
  const sum = () => {
    let running = 0;
    for (const value of values) {
      running += value;
    }
    setTotal(running);
  };
  return (
    <div className="sum" onClick={sum}>
      <span className="sum-value">{total}</span>
    </div>
  );
}
