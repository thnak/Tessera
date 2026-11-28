// SPDX-License-Identifier: MIT

// `Details`, the styled disclosure of the Instrument library (issue #1041, ADR-059 group E, the
// target of `details`/`summary`). It follows the `Disclosure` behavior primitive
// (primitives/Disclosure.tsx): a focusable `button` summary with `aria-expanded`, Enter, Space and
// pointer flip the open state, and a closed body is `display: none`, which removes it from layout,
// events and the accessibility tree. It restates the primitive instead of composing it because a
// component cannot forward its `{children}` slot into another component's slot (ir-v1.md section
// 3.4). The state is whether the reader flipped the initial state, not the state itself: a state
// initializer that reads a prop is evaluated before the parent has written its props (#1055).
// `summary` is the summary text and `defaultOpen` the initial state. Parts: `tdet`,
// `tdet-summary`, `tdet-body`, from `disclosure.css`.

import { Children, useState } from "tessera";

export type DetailsProps = { summary: string; defaultOpen?: boolean; children: Children };

export function Details({ summary, defaultOpen = false, children }: DetailsProps) {
  const [flipped, setFlipped] = useState<boolean>(false);
  const open = flipped ? !defaultOpen : defaultOpen;
  return (
    <View className={open ? "tdet tdet-open" : "tdet"}>
      <View
        role="button"
        tabIndex={0}
        aria-label={summary}
        aria-expanded={open ? "true" : "false"}
        className="tdet-summary"
        onClick={() => setFlipped(!flipped)}
      >
        <Text>{summary}</Text>
      </View>
      <View className="tdet-body" style={{ display: open ? "flex" : "none" }}>
        {children}
      </View>
    </View>
  );
}
