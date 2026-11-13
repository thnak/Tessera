// SPDX-License-Identifier: MIT

// `DismissableLayer`, the dismissable-layer behavior primitive (issue #236). It draws nothing: one
// `dialog` View that owns the open state and closes on Escape while it, or a descendant, holds
// focus. `display: none` removes it from layout, events and the accessibility tree. Focus restore belongs
// to the composer: it holds a `ref` to the opener and calls `opener.focus()` where it closes the
// layer (a ref cannot cross a component boundary in the JSX subset).

import { Children, KeyEvent, useState } from "tessera";

export type DismissableLayerProps = {
  label: string;
  defaultOpen?: boolean;
  children: Children;
};

export function DismissableLayer({ label, defaultOpen = true, children }: DismissableLayerProps) {
  const [open, setOpen] = useState<boolean>(defaultOpen);
  return (
    <View
      role="dialog"
      tabIndex={-1}
      aria-label={label}
      style={{ display: open ? "flex" : "none" }}
      onKeyDown={(e: KeyEvent) => e.key === "Escape" && setOpen(false)}
    >
      {children}
    </View>
  );
}
