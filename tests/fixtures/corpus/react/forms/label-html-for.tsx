// SPDX-License-Identifier: MIT

// Corpus fixture: `label htmlFor`. Supported since issue #542 (ADR-059 group B): it lowers to `aria-for`, and
// a click on the label focuses and activates the control with that id.

export function LabelledField({ id, label }: { id: string; label: string }) {
  return (
    <div className="labelled-field">
      <label htmlFor={id}>{label}</label>
      <input id={id} />
    </div>
  );
}
