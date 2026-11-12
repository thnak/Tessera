import { useEffect, useState } from "react";
import ReactMarkdown, { type Components } from "react-markdown";
import rehypeSlug from "rehype-slug";
import remarkGfm from "remark-gfm";
import { hasDoc, loadImage, repoBlobBase, resolveRepoPath } from "./content";

export function docHref(path: string, anchor?: string): string {
  return `#/doc/${path}${anchor ? `?h=${encodeURIComponent(anchor)}` : ""}`;
}

function isExternal(href: string): boolean {
  return /^([a-z][a-z0-9+.-]*:|\/\/)/i.test(href);
}

function RepoImage({ from, src, alt }: { from: string; src: string; alt?: string }) {
  const [url, setUrl] = useState<string | undefined>();
  const path = isExternal(src) ? null : resolveRepoPath(from, src);
  useEffect(() => {
    setUrl(undefined);
    if (path === null) return;
    void loadImage(path)?.then(setUrl);
  }, [path]);
  if (path === null) return <img src={src} alt={alt} />;
  return <img src={url ?? repoBlobBase + path + "?raw=true"} alt={alt} />;
}

export function Markdown({ source, from }: { source: string; from: string }) {
  const components: Components = {
    a({ href, children }) {
      if (href === undefined) return <>{children}</>;
      if (href.startsWith("#")) return <a href={docHref(from, href.slice(1))}>{children}</a>;
      if (isExternal(href)) {
        return (
          <a href={href} target="_blank" rel="noreferrer">
            {children}
          </a>
        );
      }
      const [file, anchor] = href.split("#");
      const target = resolveRepoPath(from, file ?? "");
      if (target !== null && hasDoc(target)) return <a href={docHref(target, anchor)}>{children}</a>;
      // Source files and anything outside docs/ and decisions/ open on GitHub.
      return (
        <a href={repoBlobBase + (target ?? file)} target="_blank" rel="noreferrer">
          {children}
        </a>
      );
    },
    img({ src, alt }) {
      return <RepoImage from={from} src={src ?? ""} alt={alt} />;
    },
  };
  return (
    <ReactMarkdown remarkPlugins={[remarkGfm]} rehypePlugins={[rehypeSlug]} components={components}>
      {source}
    </ReactMarkdown>
  );
}
