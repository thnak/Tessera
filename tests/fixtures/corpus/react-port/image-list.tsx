// SPDX-License-Identifier: MIT

// Corpus (spike #660): image elements with alt and src

export function Gallery({ urls }: { urls: string[] }) {
  return (
    <div className="gallery">
      {urls.map((u) => (
        <img key={u} src={u} alt="" width={64} height={64} />
      ))}
    </div>
  );
}
