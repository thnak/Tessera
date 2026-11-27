// SPDX-License-Identifier: MIT

// Corpus (spike #660): effect that writes document.title

import { useEffect } from "react";

export function PageTitle({ title }: { title: string }) {
  useEffect(() => {
    document.title = title;
  }, [title]);
  return <h1>{title}</h1>;
}
