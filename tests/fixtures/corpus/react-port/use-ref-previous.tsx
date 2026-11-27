// SPDX-License-Identifier: MIT

// Corpus (spike #660): useRef holding the previous value

import { useEffect, useRef } from "react";

export function Delta({ value }: { value: number }) {
  const prev = useRef(value);
  useEffect(() => {
    prev.current = value;
  });
  return <span>{value - prev.current}</span>;
}
