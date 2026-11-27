// SPDX-License-Identifier: MIT

// Corpus (spike #660): useReducer with a switch reducer

import { useReducer } from "react";

interface State {
  count: number;
}

type Action = { type: "inc" } | { type: "dec" } | { type: "reset" };

function reducer(state: State, action: Action): State {
  switch (action.type) {
    case "inc":
      return { count: state.count + 1 };
    case "dec":
      return { count: state.count - 1 };
    case "reset":
      return { count: 0 };
  }
}

export function ReducerCounter() {
  const [state, dispatch] = useReducer(reducer, { count: 0 });
  return (
    <div>
      <span>{state.count}</span>
      <button onClick={() => dispatch({ type: "inc" })}>+</button>
      <button onClick={() => dispatch({ type: "dec" })}>-</button>
      <button onClick={() => dispatch({ type: "reset" })}>reset</button>
    </div>
  );
}
