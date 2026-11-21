// SPDX-License-Identifier: MIT

// Corpus fixture: a helper function declared at module scope next to the component. Not in
// `react-core-coverage.md`: every `function` declaration is parsed as a component, so a lowercase
// helper is rejected as a component name and a helper that returns a value has no `return <element>`.
// The port moves the helper into a native handler or a `const` arrow.

function formatLabel(value: string): string {
  return value.trim();
}

export function Label({ value }: { value: string }) {
  return (
    <div className="label">
      <span className="label-text">{formatLabel(value)}</span>
    </div>
  );
}
