// SPDX-License-Identifier: MIT

// The device's top band: a dark hatched tab hanging from the top edge (asymmetric radius), with a
// chamfered brand mark, the title and a micro subtitle, skewed accent lines, status LEDs and the
// actions. Parts: `tappbar`, `tappbar-brand`, `tappbar-mark`, `tappbar-title`, `tappbar-sub`,
// `tappbar-trace` (decorative lines), `tappbar-leds`, `tappbar-actions`.
import type { ReactNode } from "react";
import "./shell.css";

export type AppBarLed = { label: string; state: "ok" | "on" | "warn" | "off" };

export type AppBarProps = {
  title: string;
  subtitle: string;
  leds: AppBarLed[];
  actions: ReactNode;
};

const ledClass = { ok: "m-led-ok", on: "m-led-on", warn: "m-led-warn", off: "" };

export function AppBar({ title, subtitle, leds, actions }: AppBarProps) {
  return (
    <header className="tappbar m-inverse m-hatch">
      <span className="tappbar-trace tappbar-trace-a" aria-hidden="true" />
      <span className="tappbar-trace tappbar-trace-b" aria-hidden="true" />
      <span className="tappbar-trace tappbar-trace-c" aria-hidden="true" />
      <div className="tappbar-brand">
        <span className="tappbar-mark m-chamfer" aria-hidden="true">
          <span className="tappbar-mark-ring" />
        </span>
        <span className="tappbar-names">
          <span className="tappbar-title m-display">{title}</span>
          <span className="tappbar-sub m-micro">{subtitle}</span>
        </span>
      </div>
      <div className="tappbar-actions">
        <ul className="tappbar-leds m-micro" aria-label="Device status">
          {leds.map((l) => (
            <li key={l.label} className="tappbar-led">
              <span className={`m-led ${ledClass[l.state]}`} aria-hidden="true" />
              {l.label}
              <span className="visually-hidden">{l.state === "off" ? " off" : " on"}</span>
            </li>
          ))}
        </ul>
        {actions}
      </div>
    </header>
  );
}
