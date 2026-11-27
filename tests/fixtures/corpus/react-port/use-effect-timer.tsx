// SPDX-License-Identifier: MIT

// Corpus (spike #660): interval timer with cleanup

import { useEffect, useState } from "react";

export function Stopwatch() {
  const [seconds, setSeconds] = useState(0);
  useEffect(() => {
    const t = setInterval(() => setSeconds((s) => s + 1), 1000);
    return () => clearInterval(t);
  }, []);
  return <span className="stopwatch">{seconds}s</span>;
}
