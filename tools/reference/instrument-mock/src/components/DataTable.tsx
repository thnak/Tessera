// SPDX-License-Identifier: MIT

// A dense read-only table with sortable columns. Real <table> semantics; sort state is
// aria-sort on the header cell and the header is a button. Numeric columns are mono, tabular and
// end-aligned. Parts: `ttable-scroll` (horizontal overflow host), `ttable`, `ttable-head`,
// `ttable-th`, `ttable-sort`, `ttable-row`, `ttable-cell` (`ttable-num` for numbers).
import { useState, type ReactNode } from "react";
import "./table.css";

export type Column<Row> = {
  key: keyof Row & string;
  label: string;
  numeric?: boolean;
  render?: (row: Row) => ReactNode;
  hideNarrow?: boolean;
};

export type DataTableProps<Row> = {
  caption: string;
  columns: Column<Row>[];
  rows: Row[];
  rowKey: (row: Row) => string;
};

export function DataTable<Row extends Record<string, string | number>>({
  caption,
  columns,
  rows,
  rowKey,
}: DataTableProps<Row>) {
  const [sortKey, setSortKey] = useState<string | null>(null);
  const [ascending, setAscending] = useState(true);

  const sorted = sortKey
    ? [...rows].sort((a, b) => {
        const x = a[sortKey];
        const y = b[sortKey];
        const order = typeof x === "number" && typeof y === "number" ? x - y : String(x).localeCompare(String(y));
        return ascending ? order : -order;
      })
    : rows;

  const toggle = (key: string) => {
    if (sortKey === key) setAscending(!ascending);
    else {
      setSortKey(key);
      setAscending(true);
    }
  };

  return (
    <div className="ttable-scroll" tabIndex={0} role="region" aria-label={caption}>
      <table className="ttable">
        <caption className="visually-hidden">{caption}</caption>
        <thead className="ttable-head">
          <tr>
            {columns.map((c) => {
              const active = sortKey === c.key;
              return (
                <th
                  key={c.key}
                  scope="col"
                  className={`ttable-th ${c.numeric ? "ttable-num" : ""} ${c.hideNarrow ? "ttable-wide" : ""}`}
                  aria-sort={active ? (ascending ? "ascending" : "descending") : undefined}
                >
                  <button type="button" className="ttable-sort m-micro" onClick={() => toggle(c.key)}>
                    {c.label}
                    <span className={`ttable-arrow ${active ? "ttable-arrow-on" : ""}`} aria-hidden="true">
                      {active && !ascending ? "▼" : "▲"}
                    </span>
                  </button>
                </th>
              );
            })}
          </tr>
        </thead>
        <tbody>
          {sorted.map((row) => (
            <tr key={rowKey(row)} className="ttable-row">
              {columns.map((c) => (
                <td
                  key={c.key}
                  className={`ttable-cell ${c.numeric ? "ttable-num mono" : ""} ${c.hideNarrow ? "ttable-wide" : ""}`}
                >
                  {c.render ? c.render(row) : row[c.key]}
                </td>
              ))}
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
