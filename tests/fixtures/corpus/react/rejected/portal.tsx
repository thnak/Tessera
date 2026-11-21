// SPDX-License-Identifier: MIT

// Corpus fixture: a portal. ADR-066 item 8 keeps this rejected: the Layer host (ADR-064 tier 0)
// and `Overlay` cover it.

import { createPortal } from "react-dom";

export function Modal({ children }: { children: string }) {
  return createPortal(
    <div className="modal">
      <span className="modal-body">{children}</span>
    </div>,
    document.body,
  );
}
