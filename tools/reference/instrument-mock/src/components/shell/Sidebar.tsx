// SPDX-License-Identifier: MIT

// Primary navigation. A <nav> of links grouped under headings; the current one has
// aria-current="page". Below the md breakpoint it becomes a horizontal strip under the app bar.
// Parts: `tsidebar`, `tsidebar-group`, `tsidebar-heading`, `tsidebar-item`
// (`tsidebar-item-current`), `tsidebar-glyph`, `tsidebar-count`.
import "./shell.css";

export type NavItem = { id: string; label: string; count?: number };
export type NavGroup = { heading: string; items: NavItem[] };

export type SidebarProps = {
  groups: NavGroup[];
  current: string;
  onNavigate: (id: string) => void;
};

export function Sidebar({ groups, current, onNavigate }: SidebarProps) {
  return (
    <nav className="tsidebar" aria-label="Screens">
      {groups.map((g) => (
        <div key={g.heading} className="tsidebar-group">
          <span className="tsidebar-heading">{g.heading}</span>
          {g.items.map((item) => {
            const on = item.id === current;
            return (
              <a
                key={item.id}
                href={`#/${item.id}`}
                className={`tsidebar-item ${on ? "tsidebar-item-current" : ""}`}
                aria-current={on ? "page" : undefined}
                onClick={(e) => {
                  e.preventDefault();
                  onNavigate(item.id);
                }}
              >
                <span className="tsidebar-glyph" aria-hidden="true" />
                <span className="tsidebar-label">{item.label}</span>
                {item.count !== undefined ? <span className="tsidebar-count mono">{item.count}</span> : null}
              </a>
            );
          })}
        </div>
      ))}
    </nav>
  );
}
