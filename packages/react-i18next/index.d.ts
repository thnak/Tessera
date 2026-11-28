// Typings of the react-i18next subset shim (docs/specs/store-i18n-shims-v1.md section 5).
// `@status pending`: recognised, a TESSERA_SHIM_PENDING diagnostic until #1046 lands.

export type TFunction = (key: string, values?: Record<string, string | number>) => string;

/** @status pending (#1046). */
export function useTranslation(): { t: TFunction };
