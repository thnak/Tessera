// SPDX-License-Identifier: MIT

// Corpus (spike #660): mouse enter and leave handlers

import { useState } from "react";

export function Hover({ text }: { text: string }) {
  const [hot, setHot] = useState(false);
  return (
    <div onMouseEnter={() => setHot(true)} onMouseLeave={() => setHot(false)}>
      {hot ? text.toUpperCase() : text}
    </div>
  );
}
