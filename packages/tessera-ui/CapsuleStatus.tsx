// SPDX-License-Identifier: MIT

// `CapsuleStatus`, the stock content of a LiveCapsule slot (issue #997, docs/specs/live-capsule-v1.md
// section 8): a status dot with an optional text label, for the compact leading/trailing slots, the
// minimal icon and the card's content. It is ordinary component content, so the capsule measures it
// like any other slot (`LiveCapsule::set_slot_measurer`): the dot is a fixed 8 px box and the label
// is one line of text, so the slot is as wide as the dot plus the text.
//
// The TSX compiler cannot yet start an activity (`LiveCapsuleHost::start` is C++, and so is the
// overlay); this component is the part of the surface that works today, and the host binding is a
// follow-up. A decorative dot is hidden from the accessibility tree: the capsule itself carries the
// accessible name of the activity. `tone` is `accent`, `success`, `warning` or `danger`; the colors
// come from theme tokens in live-capsule.css, so a tone is never the only signal (the label says it).

export type CapsuleStatusProps = { label?: string; tone?: string; dot?: boolean };

export function CapsuleStatus({ label = "", tone = "accent", dot = true }: CapsuleStatusProps) {
  return (
    <View className={`tcaps-status tcaps-tone-${tone}`}>
      {dot && <View role="presentation" aria-hidden={true} className="tcaps-dot" />}
      {label !== "" && <Text className="tcaps-label">{label}</Text>}
    </View>
  );
}
