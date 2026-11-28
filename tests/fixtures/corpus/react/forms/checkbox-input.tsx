// SPDX-License-Identifier: MIT

// Corpus fixture: `checked` and `type="checkbox"`. The tag lowers onto the library Checkbox (#1041,
// html-aliases.md section 3.8), which the file must import; this fixture does not, so the pipeline
// reports TESSERA_IR_UNRESOLVED with the import line.

export function AcceptTerms({ accepted, onChange }: { accepted: boolean; onChange: () => void }) {
  return (
    <div className="accept-terms">
      <input type="checkbox" checked={accepted} onChange={onChange} />
      <span className="accept-label">I accept the terms</span>
    </div>
  );
}
