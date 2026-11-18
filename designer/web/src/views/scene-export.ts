// SPDX-License-Identifier: MIT

// The scene view's default layout rectangle.
//
// There is one scene export now (`tessera_scene_json(component, fold_posture, width, height, dpr)` over a
// staged project), so the page no longer chooses between plain, sized, folded and styled variants: a
// non-positive size makes the module fall back to this same rectangle at DPR 1, which the parity gate pins.

// The module's own default (wasm_scene_entry.cpp's kDefaultWidth/Height/DpiScale).
export const DEFAULT_VIEWPORT = { width: 320, height: 240 } as const;
export const DEFAULT_DPR = 1;
