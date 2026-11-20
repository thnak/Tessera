// SPDX-License-Identifier: MIT
// Tokens and the page base load before any component sheet.
import "./generated/tokens.css";
import "./styles/base.css";
import "./styles/material.css";
import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { App } from "./App";

createRoot(document.getElementById("root")!).render(
  <StrictMode>
    <App />
  </StrictMode>,
);
