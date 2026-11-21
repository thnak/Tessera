// SPDX-License-Identifier: MIT

// Corpus fixture: `StrictMode` as a pass-through wrapper. ADR-066 item 1 proposes to accept it,
// since there is no development double-render in this model.

import { StrictMode } from "react";

export function App() {
  return (
    <StrictMode>
      <div className="app">
        <h1 className="app-title">Tessera</h1>
      </div>
    </StrictMode>
  );
}
