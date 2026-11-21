// SPDX-License-Identifier: MIT

// Corpus fixture: `memo(Component)`. ADR-066 item 1 proposes to accept it as identity, because
// updates already follow dependencies in this model.

import { memo } from "react";

function Row({ label, value }: { label: string; value: string }) {
  return (
    <div className="row">
      <span className="row-label">{label}</span>
      <span className="row-value">{value}</span>
    </div>
  );
}

export const MemoRow = memo(Row);
