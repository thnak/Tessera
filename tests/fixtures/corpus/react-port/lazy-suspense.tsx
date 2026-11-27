// SPDX-License-Identifier: MIT

// Corpus (spike #660): lazy component under Suspense

import { lazy, Suspense } from "react";

const Chart = lazy(() => import("./Chart"));

export function Dashboard() {
  return (
    <Suspense fallback={<p>Loading</p>}>
      <Chart />
    </Suspense>
  );
}
