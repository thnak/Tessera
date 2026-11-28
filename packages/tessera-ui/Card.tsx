// SPDX-License-Identifier: MIT

// `Card`, the bordered surface of the Instrument library (issue #240, stage 1, ADR-047). It groups
// related content on a raised surface. `title` (optional) draws a heading line above the children;
// `variant` is `raised` (default, filled and bordered) or `flat` (border only). Parts: `tcard`,
// `tcard-<variant>`, `tcard-title` and `tcard-body` from `card.css`.
//
// Accessibility: a Card is layout, so it has no role and is not focusable. A Card that is itself
// clickable is a Button or Link wrapping its content, not a Card with a handler.

import { Children } from "tessera";

export type CardProps = { title?: string; variant?: string; children: Children };

export function Card({ title = "", variant = "raised", children }: CardProps) {
  return (
    <View className={`tcard tcard-${variant}`}>
      {title !== "" && <Text className="tcard-title">{title}</Text>}
      <View className="tcard-body">{children}</View>
    </View>
  );
}
