// SPDX-License-Identifier: MIT

// Corpus fixture: `import * as React from "react"`, the namespace spelling of the same unused
// import. ADR-066 item 1 proposes to accept and ignore it too.

import * as React from "react";

export function Chip({ label }: { label: string }) {
  return (
    <div className="chip">
      <span className="chip-label">{label}</span>
    </div>
  );
}
