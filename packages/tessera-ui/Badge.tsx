// SPDX-License-Identifier: MIT

// `Badge`, the small status pill of the Instrument library (issue #240, stage 1, ADR-047). It is
// presentational: `label` is the text and `tone` is `neutral` (default), `accent`, `success`,
// `warning` or `danger`. Parts: `tbdg` and `tbdg-<tone>` from `badge.css`, theme tokens only.
//
// Accessibility: a Badge is text inside its parent, not a control, so it has no role and is not
// focusable; its label is read in document order. Meaning must not rest on the tone alone, so the
// label states it ("Failed", not just a red dot). A count that changes live belongs in a `status`
// region the caller owns.

export type BadgeProps = { label: string; tone?: string };

export function Badge({ label, tone = "neutral" }: BadgeProps) {
  return (
    <View className={`tbdg tbdg-${tone}`}>
      <Text>{label}</Text>
    </View>
  );
}
