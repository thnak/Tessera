// SPDX-License-Identifier: MIT

// Corpus (spike #660): memo wrapping a row component

import { memo } from "react";

export const Row = memo(function Row({ label, value }: { label: string; value: number }) {
  return (
    <div className="row">
      <span>{label}</span>
      <b>{value}</b>
    </div>
  );
});
