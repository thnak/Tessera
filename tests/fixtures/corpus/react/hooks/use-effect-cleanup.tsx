// SPDX-License-Identifier: MIT

// Corpus fixture: a `useEffect` with a cleanup return and an exact dependency list. The accepted
// form of the subset; it is here so the effect rules are measured against a file that should pass.
// The `bind::` annotation is required because an effect body is a protected region in generated C++.

import { useEffect, useState } from "react";

export function Clock({ interval }: { interval: int }) {
  const [tick, setTick] = useState<int>(0);
  // Note: bind::clock_tick
  useEffect(() => {
    setTick(tick + interval);
    return () => {
      setTick(0);
    };
  }, [tick, interval]);
  return (
    <div className="clock">
      <span className="clock-tick">{tick}</span>
    </div>
  );
}
