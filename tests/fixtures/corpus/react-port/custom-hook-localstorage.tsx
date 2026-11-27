// SPDX-License-Identifier: MIT

// Corpus (spike #660): a custom hook over browser storage

import { useEffect, useState } from "react";

function useLocalStorage(key: string, fallback: string) {
  const [value, setValue] = useState(() => localStorage.getItem(key) ?? fallback);
  useEffect(() => {
    localStorage.setItem(key, value);
  }, [key, value]);
  return [value, setValue] as const;
}

export function NameField() {
  const [name, setName] = useLocalStorage("name", "");
  return <input value={name} onChange={(e) => setName(e.target.value)} />;
}
