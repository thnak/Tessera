// SPDX-License-Identifier: MIT

// `Spinner`, the indeterminate activity ring of the Instrument library (issue #240, stage 1,
// ADR-047). `label` names what is loading; `size` is `sm` (16 px) or `md` (default, 24 px). It is a static ring
// for now (motion waits for the keyframes fix, #1068, see `spinner.css`).
// Parts: `tspn` and `tspn-<size>` from `spinner.css`.
//
// Accessibility: a `progressbar` named by `label` with no `aria-valuenow`, which is how an
// indeterminate progress is exposed. It is not focusable.

export type SpinnerProps = { label: string; size?: string };

export function Spinner({ label, size = "md" }: SpinnerProps) {
  return (
    <View role="progressbar" aria-label={label} className={`tspn tspn-${size}`}>
    </View>
  );
}
