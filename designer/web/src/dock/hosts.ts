// SPDX-License-Identifier: MIT

// Persistent panel hosts. Every open panel owns one `<div data-panel="<id>">`, created once and kept for the
// life of the page. The dock library renders a *slot* where a panel's tab content goes and the slot adopts
// the host (`PanelSlot.tsx`); React renders the panel's content into the host with a portal. So the DOM a
// panel owns (the canvas, the editor) is created once per open and is never recreated by a move, a tab
// switch, a split or a resize: the library repositions the slot, and the host travels inside it.
//
// A host is only ever detached from the document while its group is collapsed or the page crosses the phone
// breakpoint; neither is a gesture on the panel itself.

const hosts = new Map<string, HTMLDivElement>();

/** The host element for a panel (created on first use), or `null` where there is no document (SSR). */
export function panelHost(id: string): HTMLDivElement | null {
  if (typeof document === "undefined") {
    return null;
  }
  let host = hosts.get(id);
  if (host === undefined) {
    host = document.createElement("div");
    host.className = "panel-host";
    host.dataset.panel = id;
    hosts.set(id, host);
  }
  return host;
}

/** Forgets a closed panel's host so a later open starts from an empty element. */
export function releasePanelHost(id: string): void {
  const host = hosts.get(id);
  host?.remove();
  hosts.delete(id);
}
