import { useMemo, useState } from "react";
import { manifest } from "./content";
import { docHref } from "./Markdown";

export function Nav({ current }: { current: string | null }) {
  const [query, setQuery] = useState("");
  const sections = useMemo(() => {
    const needle = query.trim().toLowerCase();
    const groups = new Map<string, typeof manifest>();
    for (const entry of manifest) {
      if (needle && !`${entry.title} ${entry.path}`.toLowerCase().includes(needle)) continue;
      const list = groups.get(entry.section) ?? [];
      list.push(entry);
      groups.set(entry.section, list);
    }
    return [...groups.entries()];
  }, [query]);

  return (
    <nav className="nav">
      <div className="brand">Tessera Wiki</div>
      <input
        className="search"
        type="search"
        placeholder="Search titles and paths"
        value={query}
        onChange={(event) => setQuery(event.target.value)}
      />
      {sections.map(([section, entries]) => (
        <div className="nav-section" key={section}>
          <div className="nav-section-title">{section}</div>
          {entries.map((entry) => (
            <a
              key={entry.path}
              className={entry.path === current ? "nav-link active" : "nav-link"}
              href={docHref(entry.path)}
            >
              {entry.title}
            </a>
          ))}
        </div>
      ))}
      {sections.length === 0 && <div className="status">No matches.</div>}
    </nav>
  );
}
