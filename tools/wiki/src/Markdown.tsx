import { useEffect, useState } from "react";
import ReactMarkdown, { type Components } from "react-markdown";
import rehypeSlug from "rehype-slug";
import remarkGfm from "remark-gfm";
import { findPage, loadImage, repoUrl, resolveWikiPath, sourceUrl } from "./content";

export function pageHref(name: string, anchor?: string): string {
  return `#/page/${name}${anchor ? `?h=${encodeURIComponent(anchor)}` : ""}`;
}

function isExternal(href: string): boolean {
  return /^([a-z][a-z0-9+.-]*:|\/\/)/i.test(href);
}

// GitHub Wiki links: [[Page]] and [[Text|Page]] become ordinary markdown links, fenced code is left alone.
function expandWikiLinks(source: string): string {
  let inFence = false;
  return source
    .split("\n")
    .map((line) => {
      if (/^\s*(```|~~~)/.test(line)) inFence = !inFence;
      if (inFence) return line;
      return line.replace(/\[\[([^\]|]+)(?:\|([^\]]+))?\]\]/g, (_all, first: string, second?: string) => {
        const target = second ?? first;
        return `[${first}](${encodeURI(target.trim().replace(/\s+/g, "-"))})`;
      });
    })
    .join("\n");
}

function RepoImage({ from, src, alt }: { from: string; src: string; alt?: string }) {
  const [url, setUrl] = useState<string | undefined>();
  const path = isExternal(src) ? null : resolveWikiPath(from, src);
  useEffect(() => {
    setUrl(undefined);
    if (path === null) return;
    void loadImage(path)?.then(setUrl);
  }, [path]);
  if (path === null) return <img src={src} alt={alt} />;
  return <img src={url} alt={alt} />;
}

export function Markdown({ source, from }: { source: string; from: string }) {
  const components: Components = {
    a({ href, children }) {
      if (href === undefined) return <>{children}</>;
      if (href.startsWith("#")) return <a href={pageHref(from, href.slice(1))}>{children}</a>;
      if (isExternal(href)) {
        return (
          <a href={href} target="_blank" rel="noreferrer">
            {children}
          </a>
        );
      }
      const [file = "", anchor] = decodeURI(href).split("#");
      const source = sourceUrl(file);
      if (source !== null) {
        return (
          <a href={source} target="_blank" rel="noreferrer">
            {children}
          </a>
        );
      }
      const target = resolveWikiPath(from, file.replace(/\.md$/, ""));
      const page = target === null ? undefined : findPage(target);
      if (page !== undefined) return <a href={pageHref(page, anchor)}>{children}</a>;
      // Unknown target: GitHub Wiki would offer to create the page, so send the reader there.
      return (
        <a href={`${repoUrl}/wiki/${encodeURI(file)}`} target="_blank" rel="noreferrer">
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
      {expandWikiLinks(source)}
    </ReactMarkdown>
  );
}
