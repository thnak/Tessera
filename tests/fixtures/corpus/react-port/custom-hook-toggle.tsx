// SPDX-License-Identifier: MIT

// Corpus (spike #660): a custom hook in the same file

import { useState } from "react";

function useToggle(initial: boolean) {
  const [value, setValue] = useState(initial);
  const toggle = () => setValue(!value);
  return [value, toggle] as const;
}

export function Disclosure({ title }: { title: string }) {
  const [open, toggle] = useToggle(false);
  return (
    <div>
      <button onClick={toggle}>{title}</button>
      {open && <p>Details</p>}
    </div>
  );
}
