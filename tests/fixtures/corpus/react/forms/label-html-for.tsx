// SPDX-License-Identifier: MIT

// Corpus fixture: `label htmlFor`. ADR-066 section 3 keeps this rejected (Q11): the association
// becomes the Field stack's job.

export function LabelledField({ id, label }: { id: string; label: string }) {
  return (
    <div className="labelled-field">
      <label htmlFor={id}>{label}</label>
      <input id={id} />
    </div>
  );
}
