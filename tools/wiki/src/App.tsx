import { useEffect, useState } from "react";
import { findPage, loadPage, manifest, repoUrl } from "./content";
import { Markdown } from "./Markdown";
import { Nav } from "./Nav";
import { useRoute } from "./route";

export function App() {
  const route = useRoute();
  const requested = route.page ?? manifest.home;
  const page = requested === null ? undefined : findPage(requested);
  const [source, setSource] = useState<string | null>(null);

  useEffect(() => {
    setSource(null);
    const pending = page === undefined ? undefined : loadPage(page);
    if (pending === undefined) return;
    let cancelled = false;
    void pending.then((text) => {
      if (!cancelled) setSource(text);
    });
    return () => {
      cancelled = true;
    };
  }, [page]);

  useEffect(() => {
    if (source === null) return;
    const target = route.anchor === null ? null : document.getElementById(route.anchor);
    if (target !== null) target.scrollIntoView();
    else window.scrollTo(0, 0);
  }, [source, route.anchor]);

  return (
    <div className="app">
      <Nav current={page ?? null} />
      <main className="main">
        <article className="article">
          {requested === null && <p className="status">The wiki has no pages yet.</p>}
          {requested !== null && page === undefined && <p className="status">Page not found: {requested}</p>}
          {page !== undefined && source === null && <p className="status">Loading...</p>}
          {page !== undefined && source !== null && (
            <>
              <Markdown source={source} from={page} />
              <p className="status edit-link">
                <a href={`${repoUrl}/wiki/${encodeURI(page)}/_edit`} target="_blank" rel="noreferrer">
                  Edit this page on GitHub
                </a>
              </p>
            </>
          )}
        </article>
      </main>
    </div>
  );
}
