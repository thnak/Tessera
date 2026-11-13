// SPDX-License-Identifier: MIT

// `FocusScope`, the focus-scope behavior primitive (issue #236). It draws nothing: one `group` View
// with `focusTrap`. While it is visible Tab stays inside it, focus moves into it when it opens, and
// the focus held before it opened is restored when it hides or goes away (the runtime does this
// through `FocusManager::sync_focus_trap`). A scope nested in another wins until it closes.
// Compose it inside `DismissableLayer` for a modal dialog.

import { Children } from "tessera";

export type FocusScopeProps = {
  label: string;
  children: Children;
};

export function FocusScope({ label, children }: FocusScopeProps) {
  return (
    <View role="group" aria-label={label} focusTrap>
      {children}
    </View>
  );
}
