// SPDX-License-Identifier: MIT

// Which panel a dock tab button belongs to. The dock library names its tab buttons `flexlayout-tabbutton-<id>`;
// the adapter and the panel-tab context menu both read the panel id back from the DOM through this one function.

/** The panel id of the dock tab `element` is in, or `null` when it is not inside a dock tab. */
export function panelOfTab(element: Element | null): string | null {
  const tab = element?.closest("[role=tab]");
  if (tab === null || tab === undefined || tab.closest(".flexlayout__tab_button") === null) {
    return null;
  }
  const id = tab.getAttribute("id") ?? "";
  const match = /flexlayout-tabbutton-(.+)$/.exec(id) ?? /tabbutton[-_](.+)$/.exec(id);
  return match === null ? (tab.getAttribute("data-panel-tab") ?? null) : match[1];
}
