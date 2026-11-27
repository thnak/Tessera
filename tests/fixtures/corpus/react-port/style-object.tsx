// SPDX-License-Identifier: MIT

// Corpus (spike #660): inline style object with computed values

export function Bar({ percent }: { percent: number }) {
  return (
    <div style={{ width: "100%", height: 8, background: "#eee" }}>
      <div style={{ width: percent + "%", height: 8, background: "#09f" }} />
    </div>
  );
}
