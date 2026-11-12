import { useEffect, useState } from "react";
import { loadDoc, manifest } from "./content";
import { Markdown } from "./Markdown";
import { Nav } from "./Nav";
import { useRoute } from "./route";

const defaultDoc = manifest.find((entry) => entry.path === "docs/README.md")?.path ?? manifest[0]?.path ?? null;

export function App() {
  const route = useRoute();
  const path = route.path ?? defaultDoc;
  const [source, setSource] = useState<string | null>(null);
  const [failed, setFailed] = useState(false);

  useEffect(() => {
    setSource(null);
    setFailed(false);
    const pending = path === null ? undefined : loadDoc(path);
    if (pending === undefined) {
      setFailed(true);
      return;
    }
    let cancelled = false;
    pending.then(
      (text) => !cancelled && setSource(text),
      () => !cancelled && setFailed(true),
    );
    return () => {
      cancelled = true;
    };
  }, [path]);

  useEffect(() => {
    if (source === null) return;
    const target = route.anchor === null ? null : document.getElementById(route.anchor);
    if (target !== null) target.scrollIntoView();
    else window.scrollTo(0, 0);
  }, [source, route.anchor]);

  return (
    <div className="app">
      <Nav current={path} />
      <main className="main">
        <article className="article">
          {failed && <p className="status">Document not found: {path}</p>}
          {!failed && source === null && <p className="status">Loading...</p>}
          {source !== null && path !== null && <Markdown source={source} from={path} />}
        </article>
      </main>
    </div>
  );
}
