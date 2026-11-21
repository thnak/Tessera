// SPDX-License-Identifier: MIT

// Corpus fixture: `cond && <X />` and `cond ? <A /> : <B />`. Both are accepted today; the fixture
// keeps the accepted forms in the corpus so the accepted share is not measured only on rejections.

export function StatusLine({ online, label }: { online: boolean; label: string }) {
  return (
    <div className="status-line">
      {online && <span className="status-on">Online</span>}
      {online ? <span className="status-yes">{label}</span> : <span className="status-no">Offline</span>}
    </div>
  );
}
