// SPDX-License-Identifier: MIT

// `Disclosure`, the `details`/`summary` behavior primitive (issue #544, ADR-059 group E). It draws
// nothing beyond structure: a root View holding the focusable summary (a `button`
// with `aria-expanded`, the `summary` text from the prop) and the content View that the children
// slot fills. Pointer, Enter and Space on the summary arrive as `onClick` and flip the open state;
// a closed content View is `display: none`, which removes it from layout, events and the
// accessibility tree. The open state is exposed as the class `open` for the disclosure marker.

import { Children, useState } from "tessera";

export type DisclosureProps = { summary: string; defaultOpen?: boolean; children: Children };

export function Disclosure({ summary, defaultOpen = false, children }: DisclosureProps) {
  const [open, setOpen] = useState<boolean>(defaultOpen);
  return (
    <View className={open ? "disclosure open" : "disclosure"}>
      <View
        role="button"
        tabIndex={0}
        aria-label={summary}
        aria-expanded={open ? "true" : "false"}
        className="disclosure-summary"
        onClick={() => setOpen(!open)}
      >
        <Text>{summary}</Text>
      </View>
      <View style={{ display: open ? "flex" : "none" }}>{children}</View>
    </View>
  );
}
