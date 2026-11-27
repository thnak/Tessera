// SPDX-License-Identifier: MIT

// Corpus (spike #660): useMemo over an array reduce

import { useMemo } from "react";

interface Line {
  price: number;
  qty: number;
}

export function Total({ lines }: { lines: Line[] }) {
  const total = useMemo(() => lines.reduce((sum, l) => sum + l.price * l.qty, 0), [lines]);
  return <p className="total">Total: {total.toFixed(2)}</p>;
}
