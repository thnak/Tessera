// Typings of the i18next subset shim (docs/specs/store-i18n-shims-v1.md section 5).
// `@status pending`: recognised, a TESSERA_SHIM_PENDING diagnostic until #1046 lands. Import by
// name (`import { t } from "i18next"`); the default import is outside the TSX subset.

/** @status pending (#1046). */
export function t(key: string, values?: Record<string, string | number>): string;
/** @status pending (#1046). */
export function changeLanguage(language: string): void;
