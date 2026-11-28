// SPDX-License-Identifier: MIT

// `Icon`, the vector icon of the Instrument library (issue #530, ADR-047). It draws one icon of the
// emitted set (`icons/IconGlyph.tsx`, produced by `tessera icons add`) by `<set>:<name>`, in size
// `sm` (16 px) or `md` (20 px). The glyph takes the text color of its parent: it is a `currentColor`
// `Path` tinted with the node's computed `color`, so there is no per-icon color prop.
//
// Accessibility: an icon that carries meaning takes a `label` and is an `img` with that name; an icon
// without one is decorative (a `presentation` box with `aria-hidden`), hidden from the accessibility tree. `mirror` marks a directional icon
// (a chevron, an arrow): it flips in right-to-left layouts through `--icon-flip` (icon.css), and a
// non-directional icon never flips. An unknown name draws nothing and keeps its box.

import { IconGlyph } from "./icons/IconGlyph";

export type IconProps = {
  name: string;
  label?: string;
  size?: string;
  mirror?: boolean;
};

export function Icon({ name, label = "", size = "md", mirror = false }: IconProps) {
  return (
    <View className={`ticon ticon-${size} ${mirror ? "ticon-mirror" : "ticon-fixed"}`}>
      {label !== "" && (
        <View role="img" aria-label={label} className="ticon-box">
          <IconGlyph name={name} />
        </View>
      )}
      {label === "" && (
        <View role="presentation" aria-hidden={true} className="ticon-box">
          <IconGlyph name={name} />
        </View>
      )}
    </View>
  );
}
