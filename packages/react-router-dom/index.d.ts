// Typings of the react-router-dom shim (docs/specs/router-shim-v1.md). Only the names of the
// coverage table are declared, so an editor flags the rest before the compiler does. `@status` is the
// table's status column: `supported` lowers today, `pending` is recognised and is a
// TESSERA_SHIM_PENDING diagnostic naming the form to use meanwhile (#1044).

/** @status supported. Call in a handler or an effect; never while rendering. */
export type NavigateFunction = {
  (to: string, options?: { replace?: boolean }): void;
  (delta: -1): void;
};

/** @status supported. Lowers to `useService<RouterService>()`. */
export function useNavigate(): NavigateFunction;

/** @status pending (#1044). */
export function useParams(): Record<string, string>;
/** @status pending (#1044). */
export function useLocation(): { pathname: string };
/** @status pending (#1044). */
export function Routes(props: { children: unknown }): unknown;
/** @status pending (#1044). */
export function Route(props: { path: string; element: unknown }): unknown;
/** @status pending (#1044). */
export function Outlet(): unknown;
/** @status pending (#1044, #542). */
export function Link(props: { to: string; children?: unknown }): unknown;
/** @status pending (#1044, #542). */
export function NavLink(props: { to: string; end?: boolean; children?: unknown }): unknown;
