// SPDX-License-Identifier: MIT

// Corpus (spike #660): object state updated with spread

import { useState } from "react";

interface Form {
  first: string;
  last: string;
}

export function NameForm() {
  const [form, setForm] = useState<Form>({ first: "", last: "" });
  return (
    <div>
      <input value={form.first} onChange={(e) => setForm({ ...form, first: e.target.value })} />
      <input value={form.last} onChange={(e) => setForm({ ...form, last: e.target.value })} />
      <p>{form.first} {form.last}</p>
    </div>
  );
}
