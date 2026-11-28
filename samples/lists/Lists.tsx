// SPDX-License-Identifier: MIT

// Lists and conditional rendering (issue #607): a keyed `.map` over a list-typed state cell, a
// conditional child and a button that changes state. The preview takes one component per file, so a
// row is an element, not a child component. Browser twin: tools/parity/lists.html.
// Limit today: a loop item is not yet usable inside an event handler (TESSERA_IR_UNRESOLVED), so a
// row cannot select itself; a top-level array constant is not lowered either, hence the state cell.

import { useState } from "tessera";
import "./lists.css";

export function Lists() {
  const [tasks, setTasks] = useState<string[]>(["Write the TSX", "Style it", "Compile to C++", "Ship"]);
  const [done, setDone] = useState<int>(0);

  return (
    <div className="board">
      <h1>Release checklist</h1>
      <ul className="tasks">
        {tasks.map((task) => (
          <li key={task} className="task">
            <span className="name">{task}</span>
          </li>
        ))}
      </ul>
      <button className="pick" onClick={() => setDone(done + 1)}>
        Mark a step done
      </button>
      {done > 0 && <p className="status">Steps done: {done}</p>}
    </div>
  );
}
