// SPDX-License-Identifier: MIT

// Corpus (spike #660): useId pairing a label and an input

import { useId } from "react";

export function Field({ label }: { label: string }) {
  const id = useId();
  return (
    <div>
      <label htmlFor={id}>{label}</label>
      <input id={id} />
    </div>
  );
}
