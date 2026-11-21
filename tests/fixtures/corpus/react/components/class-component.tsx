// SPDX-License-Identifier: MIT

// Corpus fixture: a class component. ADR-066 item 8 keeps this rejected by design (ADR-036), so it
// is measured to confirm the rejection is deliberate and not an accident of the parser.

import React from "react";

export class Counter extends React.Component<{ start: int }> {
  state = { count: this.props.start };

  render() {
    return (
      <div className="counter">
        <span className="counter-value">{this.state.count}</span>
      </div>
    );
  }
}
