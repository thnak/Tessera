// SPDX-License-Identifier: MIT

// A modal dialog on the native <dialog>: showModal() gives the focus trap, Escape, the top layer and
// inert background (the FocusScope + DismissableLayer contract); closing returns focus to the opener.
// Drawn as a small device face: chassis gradient, corner screws, an engraved head with an index badge,
// and the actions in a recessed tray. Parts: `tdialog` (root/backdrop host), `tdialog-header`,
// `tdialog-index`, `tdialog-title`, `tdialog-close`, `tdialog-body`, `tdialog-actions`.
import { useEffect, useId, useRef, type ReactNode } from "react";
import "./dialog.css";

export type DialogProps = {
  open: boolean;
  title: string;
  index?: string;
  onClose: () => void;
  actions: ReactNode;
  children: ReactNode;
};

export function Dialog({ open, title, index = "00", onClose, actions, children }: DialogProps) {
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
        <span className="m-screw m-screw-tl" aria-hidden="true" />
        <span className="m-screw m-screw-tr" aria-hidden="true" />
        <span className="m-screw m-screw-bl" aria-hidden="true" />
        <span className="m-screw m-screw-br" aria-hidden="true" />
        <div className="tdialog-header">
          <span className="tdialog-index m-index">{index}</span>
          <h2 className="tdialog-title m-display" id={titleId}>
            {title}
          </h2>
          <span className="m-engrave" aria-hidden="true" />
          <button type="button" className="tdialog-close m-raised" aria-label="Close" onClick={onClose}>
            <span className="tdialog-close-glyph" aria-hidden="true" />
          </button>
        </div>
        <div className="tdialog-body">{children}</div>
        <div className="tdialog-actions m-recess">{actions}</div>
      </div>
    </dialog>
  );
}
