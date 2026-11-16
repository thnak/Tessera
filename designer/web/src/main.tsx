// SPDX-License-Identifier: MIT

// The studio page's browser entry point (ADR-054 decision 1; issue #422).
//
// It mounts the React tree into the one element `index.html` carries and does nothing else. Everything the
// page needs -- the toolbar, the three view sessions, the Wasm modules -- is behind `App`, so this file is
// the only place that knows a document exists, and the chrome can be rendered without a browser by the Node
// gates (`src/ssr-chrome.tsx`).

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import "./styles/app.css";

const container = document.getElementById("root");
if (!container) {
  throw new Error("the page has no #root element to mount the studio into");
}

createRoot(container).render(
  // StrictMode in development only: React 19 double-invokes effects under it, and a session that creates a
  // WebGPU device is exactly the kind of effect that must not run twice. Vite replaces this with `false` in
  // a production build, which is what ships.
  <StrictMode>
    <App />
  </StrictMode>,
);
