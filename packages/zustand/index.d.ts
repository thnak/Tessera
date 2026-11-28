// Typings of the zustand-shaped store shim (docs/specs/store-i18n-shims-v1.md section 3).
// `@status pending`: recognised, a TESSERA_SHIM_PENDING diagnostic until #1045 lands.

type SetState<T> = (partial: Partial<T> | ((state: T) => Partial<T>)) => void;
type GetState<T> = () => T;

/** @status pending (#1045). Curried form only: `create<State>()((set, get) => ({ ... }))`. */
export function create<T>(): (
  initializer: (set: SetState<T>, get: GetState<T>) => T,
) => <U>(selector: (state: T) => U) => U;
