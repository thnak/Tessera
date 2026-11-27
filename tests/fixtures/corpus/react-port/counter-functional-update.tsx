// SPDX-License-Identifier: MIT

// Corpus (spike #660): functional state update with the previous value

import { useState } from "react";

export default function Clicker() {
  const [clicks, setClicks] = useState<number>(0);
  return (
    <button className="clicker" onClick={() => setClicks((prev) => prev + 1)}>
      Clicked {clicks} times
    </button>
  );
}
