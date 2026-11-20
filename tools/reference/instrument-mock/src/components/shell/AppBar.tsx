// SPDX-License-Identifier: MIT

// The window's top bar. Parts: `tappbar`, `tappbar-brand`, `tappbar-mark`, `tappbar-title`,
// `tappbar-crumb` (mono path of the current screen), `tappbar-actions`.
import type { ReactNode } from "react";
import "./shell.css";

export type AppBarProps = {
  title: string;
  crumb: string;
  actions: ReactNode;
};

export function AppBar({ title, crumb, actions }: AppBarProps) {
  return (
    <header className="tappbar">
      <div className="tappbar-brand">
        <span className="tappbar-mark" aria-hidden="true" />
        <span className="tappbar-title">{title}</span>
        <span className="tappbar-crumb mono" aria-hidden="true">
          / {crumb}
        </span>
      </div>
      <div className="tappbar-actions">{actions}</div>
    </header>
  );
}
