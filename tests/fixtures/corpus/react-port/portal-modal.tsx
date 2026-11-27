// SPDX-License-Identifier: MIT

// Corpus (spike #660): modal rendered through a portal

import { createPortal } from "react-dom";

export function Modal({ open, children }: { open: boolean; children: React.ReactNode }) {
  if (!open) return null;
  return createPortal(<div className="modal">{children}</div>, document.body);
}
