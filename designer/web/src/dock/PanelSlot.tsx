// SPDX-License-Identifier: MIT

import { useEffect, useRef } from "react";
import { panelHost } from "./hosts";

/** Where the dock library puts a panel's content: adopts the panel's persistent host element. */
export function PanelSlot({ panelId }: { panelId: string }) {
  const slot = useRef<HTMLDivElement>(null);
  useEffect(() => {
    const element = slot.current;
    const host = panelHost(panelId);
    if (element === null || host === null) {
      return;
    }
    element.appendChild(host);
    return () => {
      if (host.parentNode === element) {
        element.removeChild(host);
      }
    };
  }, [panelId]);
  return <div ref={slot} className="panel-slot" data-panel-slot={panelId} />;
}
