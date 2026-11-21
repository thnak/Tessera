// SPDX-License-Identifier: MIT

// Corpus fixture: `useReducer` with a `switch` reducer over a record state. ADR-066 item 4
// proposes the limited form; this is the shape it is written for.

import { useReducer } from "react";

type CounterState = { count: int };
type CounterAction = { type: "increment" } | { type: "reset" };

function reduce(state: CounterState, action: CounterAction): CounterState {
  switch (action.type) {
    case "increment":
      return { count: state.count + 1 };
    case "reset":
      return { count: 0 };
  }
}

export function Counter() {
  const [state, dispatch] = useReducer(reduce, { count: 0 });
  return (
    <div className="counter">
      <span className="counter-value">{state.count}</span>
      <button onClick={() => dispatch({ type: "increment" })}>Add</button>
    </div>
  );
}
