// SPDX-License-Identifier: MIT

// Corpus (spike #660): useState counter with number types

import { useState } from "react";

interface CounterProps {
  initial?: number;
  step?: number;
}

export function Counter({ initial = 0, step = 1 }: CounterProps) {
  const [count, setCount] = useState(initial);
  return (
    <div className="counter">
      <button onClick={() => setCount(count - step)}>-</button>
      <span>{count}</span>
      <button onClick={() => setCount(count + step)}>+</button>
    </div>
  );
}
