// SPDX-License-Identifier: MIT

// Motion (issue #607): `transition` on :hover / :active, a looping `@keyframes` pulse, and a state
// change that switches the class a transition runs against. Browser twin: tools/parity/motion.html.
// TODO(#603): `useAnimatedValue` and springs do not play in the Studio yet; they are not faked here.

import { useState } from "tessera";
import "./motion.css";

export function Motion() {
  const [open, setOpen] = useState<bool>(false);

  return (
    <div className="stage">
      <h1>Motion</h1>
      <div className="pulse" />
      <button className="toggle" onClick={() => setOpen(!open)}>
        {open ? "Collapse" : "Expand"}
      </button>
      <div className={open ? "drawer drawer-open" : "drawer"}>
        <p>This panel's height and colour transition when the class changes.</p>
      </div>
    </div>
  );
}
