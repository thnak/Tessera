// SPDX-License-Identifier: MIT

// `Skeleton`, the loading placeholder of the Instrument library (issue #240, stage 1, ADR-047).
// `shape` is `text` (default, one line), `rect` (a block) or `circle` (an avatar). It is static
// for now (the pulse waits for the keyframes fix, #1068, see `skeleton.css`). Parts: `tsk` and `tsk-<shape>` from
// `skeleton.css`.
//
// Accessibility: hidden from the accessibility tree. The region that loads names the state (an
// `aria-busy` container or a `status` message the caller owns); a placeholder shape says nothing.

export type SkeletonProps = { shape?: string };

export function Skeleton({ shape = "text" }: SkeletonProps) {
  return (
    <View role="presentation" aria-hidden={true} className={`tsk tsk-${shape}`}>
    </View>
  );
}
