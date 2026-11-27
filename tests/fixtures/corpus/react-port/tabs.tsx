// SPDX-License-Identifier: MIT

// Corpus (spike #660): tab bar with index state and ternary class

import { useState } from "react";

const TABS = ["Overview", "Details", "History"];

export function Tabs() {
  const [active, setActive] = useState(0);
  return (
    <nav>
      {TABS.map((t, i) => (
        <button key={t} className={i === active ? "tab active" : "tab"} onClick={() => setActive(i)}>
          {t}
        </button>
      ))}
    </nav>
  );
}
