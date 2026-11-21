// SPDX-License-Identifier: MIT

// Corpus fixture: `.length` on a list. ADR-066 item 3 proposes `length(list)` as the Tessera form;
// the coverage table records that the function exists and the property does not.

export function RowCount({ rows }: { rows: string[] }) {
  return (
    <div className="row-count">
      <span className="row-count-value">{rows.length}</span>
    </div>
  );
}
