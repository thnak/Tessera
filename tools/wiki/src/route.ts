import { useEffect, useState } from "react";

export interface Route {
  page: string | null;
  anchor: string | null;
}

export function parseHash(hash: string): Route {
  const match = /^#\/page\/([^?]+)(?:\?h=(.*))?$/.exec(hash);
  if (match === null) return { page: null, anchor: null };
  return { page: decodeURI(match[1] ?? ""), anchor: match[2] ? decodeURIComponent(match[2]) : null };
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
