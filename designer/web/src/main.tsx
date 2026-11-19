// SPDX-License-Identifier: MIT

// The studio page's browser entry point (ADR-054 decision 1; issue #422; the three-column workspace is
// issue #431).
//
// It mounts the React tree into the one element `index.html` carries and imports the stylesheets. Since
// #431 the sheet is split by region (`styles/*.css`) so no file is near the 500-line limit; Vite still
// emits **one** content-hashed `assets/index-<hash>.css`, so the page's CSS is one request and one file on
// the wire exactly as before. The import order is the cascade order: tokens and shared shapes first, then
// the regions, then the responsive rules.
//
// Nothing else happens here. Everything the page needs -- the tree, the tabs, the three view sessions, the
// Wasm modules -- is behind `App`, so this file is the only place that knows a document exists, and the
// chrome can be rendered without a browser by the Node gates (`src/ssr-chrome.tsx`).

import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";
import "./styles/base.css";
import "./styles/chrome.css";
import "./styles/workbench-chrome.css";
import "./styles/workspace.css";
import "./styles/code.css";
import "./styles/preview.css";
import "./styles/skin.css";
import "./styles/platform.css";
import "flexlayout-react/style/light.css";
import "./styles/dock.css";
import "./styles/problems.css";
import "./styles/workbench.css";
import "./styles/responsive.css";

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
