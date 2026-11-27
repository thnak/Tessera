// SPDX-License-Identifier: MIT

// Corpus (spike #660): async effect that fetches data

import { useEffect, useState } from "react";

export function UserCard({ id }: { id: string }) {
  const [name, setName] = useState<string | null>(null);
  useEffect(() => {
    let cancelled = false;
    async function load() {
      const res = await fetch("/api/users/" + id);
      const data = await res.json();
      if (!cancelled) setName(data.name);
    }
    load();
    return () => {
      cancelled = true;
    };
  }, [id]);
  return <div className="user-card">{name ?? "Loading..."}</div>;
}
