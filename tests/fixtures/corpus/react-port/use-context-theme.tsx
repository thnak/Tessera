// SPDX-License-Identifier: MIT

// Corpus (spike #660): createContext and useContext for a theme

import { createContext, useContext } from "react";

const ThemeContext = createContext("light");

export function ThemedLabel({ text }: { text: string }) {
  const theme = useContext(ThemeContext);
  return <span className={"label " + theme}>{text}</span>;
}

export function App() {
  return (
    <ThemeContext.Provider value="dark">
      <ThemedLabel text="Hello" />
    </ThemeContext.Provider>
  );
}
