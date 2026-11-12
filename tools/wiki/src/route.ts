import { useEffect, useState } from "react";

export interface Route {
  path: string | null;
  anchor: string | null;
}

export function parseHash(hash: string): Route {
  const match = /^#\/doc\/([^?]+)(?:\?h=(.*))?$/.exec(hash);
  if (match === null) return { path: null, anchor: null };
  return { path: decodeURI(match[1] ?? ""), anchor: match[2] ? decodeURIComponent(match[2]) : null };
}

export function useRoute(): Route {
  const [route, setRoute] = useState(() => parseHash(window.location.hash));
  useEffect(() => {
    const onChange = () => setRoute(parseHash(window.location.hash));
    window.addEventListener("hashchange", onChange);
    return () => window.removeEventListener("hashchange", onChange);
  }, []);
  return route;
}
