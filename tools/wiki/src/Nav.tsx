import { useMemo, useState } from "react";
import { manifest, repoUrl } from "./content";
import { pageHref } from "./Markdown";

export function Nav({ current }: { current: string | null }) {
  const [query, setQuery] = useState("");
  const sections = useMemo(() => {
    const needle = query.trim().toLowerCase();
    return manifest.sections
      .map((section) => ({
        title: section.title,
        pages: section.pages.filter((page) => !needle || `${page.title} ${page.name}`.toLowerCase().includes(needle)),
      }))
      .filter((section) => section.pages.length > 0);
  }, [query]);

  return (
    <nav className="nav">
      <div className="brand">Tessera Wiki</div>
      <input
        className="search"
        type="search"
        placeholder="Search pages"
        value={query}
        onChange={(event) => setQuery(event.target.value)}
      />
      {sections.map((section) => (
        <div className="nav-section" key={section.title}>
          <div className="nav-section-title">{section.title}</div>
          {section.pages.map((page) => (
            <a
              key={page.name}
              className={page.name === current ? "nav-link active" : "nav-link"}
              href={pageHref(page.name)}
            >
              {page.title}
            </a>
          ))}
        </div>
      ))}
      {sections.length === 0 && <div className="status">No matches.</div>}
      <div className="nav-section">
        <div className="nav-section-title">Source</div>
        <a className="nav-link" href={`${repoUrl}/tree/main/docs`} target="_blank" rel="noreferrer">
          Design docs and specs
        </a>
        <a className="nav-link" href={`${repoUrl}/tree/main/decisions`} target="_blank" rel="noreferrer">
          Decisions (ADRs)
        </a>
      </div>
    </nav>
  );
}
