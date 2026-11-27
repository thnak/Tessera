// SPDX-License-Identifier: MIT

// Corpus (spike #660): todo list with keyed map and a controlled input

import { useState } from "react";

interface Todo {
  id: number;
  text: string;
  done: boolean;
}

export function TodoList({ items }: { items: Todo[] }) {
  const [draft, setDraft] = useState("");
  return (
    <section className="todos">
      <input value={draft} onChange={(e) => setDraft(e.target.value)} placeholder="New todo" />
      <ul>
        {items.map((item) => (
          <li key={item.id} className={item.done ? "done" : ""}>
            {item.text}
          </li>
        ))}
      </ul>
    </section>
  );
}
