// SPDX-License-Identifier: MIT

// Corpus fixture: `createContext` and `useContext` with a compile-time provider. ADR-066 item 5
// proposes the static form; the provider value here is a pure expression of the props.

import { createContext, useContext } from "react";

type Theme = { accent: string };

const ThemeContext = createContext<Theme>({ accent: "blue" });

function AccentLabel() {
  const theme = useContext(ThemeContext);
  return <span className="accent-label">{theme.accent}</span>;
}

export function ThemedPanel({ accent }: { accent: string }) {
  return (
    <ThemeContext.Provider value={{ accent }}>
      <div className="themed-panel">
        <AccentLabel />
      </div>
    </ThemeContext.Provider>
  );
}
