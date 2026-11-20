// SPDX-License-Identifier: MIT

// Primary navigation as a column of keys on the chassis: each item has a numbered index badge and an
// LED; the current one is a raised key with the LED lit (aria-current="page"). A perforated grille tab
// sits under the keys. Below the md breakpoint it becomes a horizontal segmented strip.
// Parts: `tsidebar`, `tsidebar-heading`, `tsidebar-item` (`tsidebar-item-current`), `tsidebar-index`,
// `tsidebar-label`, `tsidebar-count`, `tsidebar-grille`.
import "./shell.css";

export type NavItem = { id: string; label: string; count?: number };
export type NavGroup = { heading: string; items: NavItem[] };

export type SidebarProps = {
  groups: NavGroup[];
  current: string;
  onNavigate: (id: string) => void;
};

export function Sidebar({ groups, current, onNavigate }: SidebarProps) {
  let n = 0;
  return (
    <nav className="tsidebar" aria-label="Screens">
      {groups.map((g) => (
        <div key={g.heading} className="tsidebar-group">
          <span className="tsidebar-heading m-micro">{g.heading}</span>
          {g.items.map((item) => {
            const on = item.id === current;
            n += 1;
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
                <span className="tsidebar-index m-index" aria-hidden="true">
                  {String(n).padStart(2, "0")}
                </span>
                <span className="tsidebar-label">{item.label}</span>
                {item.count !== undefined ? <span className="tsidebar-count mono">{item.count}</span> : null}
                <span className={`m-led ${on ? "m-led-on" : ""}`} aria-hidden="true" />
              </a>
            );
          })}
        </div>
      ))}
      <span className="tsidebar-grille m-grille" aria-hidden="true" />
    </nav>
  );
}
