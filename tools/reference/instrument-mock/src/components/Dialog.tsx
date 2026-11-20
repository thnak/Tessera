// SPDX-License-Identifier: MIT

// A modal dialog on the native <dialog>: showModal() gives the focus trap, Escape, the top layer and
// inert background (the FocusScope + DismissableLayer contract); closing returns focus to the opener.
// Parts: `tdialog` (root/backdrop host), `tdialog-header`, `tdialog-title`, `tdialog-close`,
// `tdialog-body`, `tdialog-actions`.
import { useEffect, useId, useRef, type ReactNode } from "react";
import "./dialog.css";

export type DialogProps = {
  open: boolean;
  title: string;
  onClose: () => void;
  actions: ReactNode;
  children: ReactNode;
};

export function Dialog({ open, title, onClose, actions, children }: DialogProps) {
  const ref = useRef<HTMLDialogElement>(null);
  const titleId = useId();

  useEffect(() => {
    const dialog = ref.current;
    if (!dialog) return;
    if (open && !dialog.open) dialog.showModal();
    if (!open && dialog.open) dialog.close();
  }, [open]);

  return (
    <dialog
      ref={ref}
      className="tdialog"
      aria-labelledby={titleId}
      onClose={onClose}
      onClick={(e) => e.target === ref.current && onClose()}
    >
      <div className="tdialog-surface">
        <div className="tdialog-header">
          <h2 className="tdialog-title" id={titleId}>
            {title}
          </h2>
          <button type="button" className="tdialog-close focusable" aria-label="Close" onClick={onClose}>
            <span className="tdialog-close-glyph" aria-hidden="true" />
          </button>
        </div>
        <div className="tdialog-body">{children}</div>
        <div className="tdialog-actions">{actions}</div>
      </div>
    </dialog>
  );
}
