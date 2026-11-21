// SPDX-License-Identifier: MIT

// Corpus fixture: `useSyncExternalStore`. ADR-066 item 8 keeps this rejected: external state
// belongs to services, so the port uses `useObservable` or `useService` (ADR-050).

import { useSyncExternalStore } from "react";

export function OnlineStatus({ subscribe, snapshot }: { subscribe: () => void; snapshot: () => boolean }) {
  const online = useSyncExternalStore(subscribe, snapshot);
  return (
    <div className="online-status">
      <span className="online-value">{online}</span>
    </div>
  );
}
