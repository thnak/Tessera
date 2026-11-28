// SPDX-License-Identifier: MIT

// `Avatar`, the identity badge of the Instrument library (issue #240, stage 1, ADR-047). It shows
// `initials` in a circle; `size` is `sm` (24 px), `md` (default, 32 px) or `lg` (48 px). A photo
// variant waits for `Image`. Parts: `tav`, `tav-<size>` and `tav-initials` from `avatar.css`.
//
// Accessibility: an `img` named by `label` (the person's name), so the initials are not read as
// text. It is not focusable.

export type AvatarProps = { label: string; initials: string; size?: string };

export function Avatar({ label, initials, size = "md" }: AvatarProps) {
  return (
    <View role="img" aria-label={label} className={`tav tav-${size}`}>
      <Text className="tav-initials">{initials}</Text>
    </View>
  );
}
