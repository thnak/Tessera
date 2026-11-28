// SPDX-License-Identifier: MIT

// The import and export controls both settings panes share (slice W8 #565): Export fills a read-only-looking text
// box and downloads the file, Import takes a chosen file or the pasted text. The pane decides what an import
// means; this only moves text and shows the one-sentence outcome. A refusal is announced (`role="alert"`) and
// changes nothing.

import { useRef, useState } from "react";
import type { Outcome } from "../page-settings";

function download(name: string, text: string): void {
  const url = URL.createObjectURL(new Blob([text], { type: "application/json" }));
  const link = document.createElement("a");
  link.href = url;
  link.download = name;
  link.click();
  URL.revokeObjectURL(url);
}

export function JsonTransfer(props: {
  /** Used in ids and labels: `keybindings` or `layout`. */
  kind: string;
  fileName: string;
  exportText: () => string;
  onImport: (text: string) => Outcome;
  /** Called after a successful import so the pane can re-read its state. */
  onChanged: () => void;
}) {
  const { kind } = props;
  const [text, setText] = useState("");
  const [outcome, setOutcome] = useState<Outcome | null>(null);
  const file = useRef<HTMLInputElement>(null);

  const run = (source: string) => {
    const result = props.onImport(source);
    setOutcome(result);
    if (result.ok) {
      setText("");
      props.onChanged();
    }
  };

  return (
    <div className="json-transfer">
      <div className="settings-row">
        <button
          type="button"
          id={`${kind}-export`}
          onClick={() => {
            const body = props.exportText();
            setText(body);
            download(props.fileName, body);
            setOutcome({ ok: true, message: `Exported ${props.fileName}.` });
          }}
        >
          Export JSON
        </button>
        <button type="button" id={`${kind}-import-file`} onClick={() => file.current?.click()}>
          Import file...
        </button>
        <input
          ref={file}
          type="file"
          accept=".json,application/json"
          hidden
          aria-label={`${kind} JSON file`}
          onChange={(event) => {
            const chosen = event.currentTarget.files?.[0];
            event.currentTarget.value = "";
            void chosen?.text().then(run, () => setOutcome({ ok: false, message: "The file could not be read." }));
          }}
        />
      </div>
      <label className="settings-label" htmlFor={`${kind}-json`}>
        JSON (exported text appears here; paste to import)
      </label>
      <textarea id={`${kind}-json`} className="settings-json" rows={6} spellCheck={false} value={text} onChange={(event) => setText(event.target.value)} />
      <div className="settings-row">
        <button type="button" id={`${kind}-import-text`} disabled={text.trim() === ""} onClick={() => run(text)}>
          Import pasted JSON
        </button>
      </div>
      <p
        className={outcome !== null && !outcome.ok ? "settings-outcome is-error" : "settings-outcome"}
        id={`${kind}-outcome`}
        role={outcome !== null && !outcome.ok ? "alert" : "status"}
      >
        {outcome?.message ?? ""}
      </p>
    </div>
  );
}
