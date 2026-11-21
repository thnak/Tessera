// SPDX-License-Identifier: MIT

// Corpus fixture: `useState` with a typed call and a setter-only arrow handler. Both are accepted
// today; they are the control the `useMemo` and `useCallback` fixtures are read against.

import { useState } from "react";

export function Stepper({ step }: { step: int }) {
  const [value, setValue] = useState<int>(0);
  return (
    <div className="stepper">
      <span className="stepper-value">{value}</span>
      <button onClick={() => setValue(value + step)}>Next</button>
    </div>
  );
}
