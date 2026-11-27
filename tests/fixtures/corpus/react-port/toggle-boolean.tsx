// SPDX-License-Identifier: MIT

// Corpus (spike #660): boolean toggle with negation

import { useState } from "react";

export const Toggle = ({ label }: { label: string }) => {
  const [on, setOn] = useState<boolean>(false);
  return (
    <button className={on ? "toggle on" : "toggle"} onClick={() => setOn(!on)}>
      {label}: {on ? "On" : "Off"}
    </button>
  );
};
