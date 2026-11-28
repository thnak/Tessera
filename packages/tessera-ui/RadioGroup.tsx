// SPDX-License-Identifier: MIT

// `RadioGroup`, the labelled `radiogroup` container of the Instrument library (issue #240, stage 1,
// ADR-047): a column of `Radio`s (Radio.tsx) in the Children slot, parts and look in `radio.css`.
// It owns no state; the caller keeps the selected value and tells each Radio whether it is selected.

import { Children } from "tessera";

export type RadioGroupProps = { label: string; children: Children };

export function RadioGroup({ label, children }: RadioGroupProps) {
  return (
    <View role="radiogroup" aria-label={label} className="trg">
      {children}
    </View>
  );
}
