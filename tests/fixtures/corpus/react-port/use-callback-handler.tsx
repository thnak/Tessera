// SPDX-License-Identifier: MIT

// Corpus (spike #660): useCallback handler passed to a child

import { useCallback, useState } from "react";

function Button({ onPress, children }: { onPress: () => void; children: React.ReactNode }) {
  return <button onClick={onPress}>{children}</button>;
}

export function Panel() {
  const [n, setN] = useState(0);
  const inc = useCallback(() => setN((v) => v + 1), []);
  return (
    <div>
      <Button onPress={inc}>Add</Button>
      <span>{n}</span>
    </div>
  );
}
