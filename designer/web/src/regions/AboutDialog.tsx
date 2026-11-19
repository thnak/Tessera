// SPDX-License-Identifier: MIT

// Help > About (`help.about`): where the studio's title and its description live now (owner feedback on #586:
// the title bar and the footer carried them permanently, which is chrome around a code editor).
//
// An accessible modal: `role="dialog"` with `aria-modal`, named by its heading and described by its body, focus
// moves in on open, Tab cycles inside it, Escape or the Close button leaves, and focus returns to where it was.
// Only text and links; every fact is static or comes from the page's build id, so there is nothing to fetch.

import { useEffect, useRef } from "react";

const REPO_URL = "https://github.com/thnak/Tessera";
const DOCS_URL = "https://github.com/thnak/Tessera/blob/main/docs/README.md";
const ISSUE_URL = "https://github.com/thnak/Tessera/issues/new";

export function AboutDialog({ buildId, onClose }: { buildId: string; onClose: () => void }) {
  const dialog = useRef<HTMLDivElement>(null);
  const close = useRef<HTMLButtonElement>(null);
  const returnTo = useRef<Element | null>(null);

  useEffect(() => {
    returnTo.current = document.activeElement;
    close.current?.focus();
    return () => {
      if (returnTo.current instanceof HTMLElement && document.contains(returnTo.current)) {
        returnTo.current.focus();
      }
    };
  }, []);

  function onKeyDown(event: React.KeyboardEvent<HTMLDivElement>) {
    if (event.key === "Escape") {
      event.preventDefault();
      event.stopPropagation();
      onClose();
    } else if (event.key === "Tab") {
      const stops = Array.from(dialog.current?.querySelectorAll<HTMLElement>("a[href], button") ?? []);
      const first = stops[0];
      const last = stops[stops.length - 1];
      if (event.shiftKey && document.activeElement === first) {
        event.preventDefault();
        last?.focus();
      } else if (!event.shiftKey && document.activeElement === last) {
        event.preventDefault();
        first?.focus();
      }
    }
  }

  return (
    <div className="about-layer" onMouseDown={(event) => event.target === event.currentTarget && onClose()}>
      <div
        className="about"
        id="about-dialog"
        role="dialog"
        aria-modal="true"
        aria-labelledby="about-title"
        aria-describedby="about-body"
        ref={dialog}
        onKeyDown={onKeyDown}
      >
        <h2 id="about-title" className="about-title">
          Tessera Studio
        </h2>
        <div id="about-body">
          <p>
            The hosted studio for <a href={REPO_URL}>Tessera</a>: edit a project and watch it run. The preview panes are
            three Wasm modules built from this repository: <code>tessera_diagnostics</code>,{" "}
            <code>tessera_scene_preview</code> and <code>tessera_canvas</code>. Each is checked against its native
            counterpart by a CI gate, byte for byte where the output is a document.
          </p>
          <dl className="about-facts">
            <dt>Build</dt>
            <dd id="about-build">{buildId.replace(/^build:\s*/, "")}</dd>
            <dt>Status</dt>
            <dd>Pre-alpha</dd>
            <dt>Privacy</dt>
            <dd>Nothing leaves the browser: no telemetry, no network call beyond this site's own files.</dd>
          </dl>
          <p className="about-links">
            <a href={REPO_URL}>Source</a> <a href={DOCS_URL}>Documentation</a> <a href={ISSUE_URL}>Report an issue</a>
          </p>
        </div>
        <div className="about-actions">
          <button type="button" className="primary" ref={close} onClick={onClose}>
            Close
          </button>
        </div>
      </div>
    </div>
  );
}
