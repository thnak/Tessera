// SPDX-License-Identifier: MIT

// Corpus fixture: `import React from "react"` next to a component that never reads it. The
// automatic JSX runtime makes the import unused; ADR-066 item 1 proposes to accept and ignore it.

import React from "react";

export function Badge({ label }: { label: string }) {
  return (
    <div className="badge">
      <span className="badge-label">{label}</span>
    </div>
  );
}
