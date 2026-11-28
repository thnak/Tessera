// SPDX-License-Identifier: MIT

// The WebGPU surface the canvas view uses, declared here rather than pulled from `@webgpu/types`.
//
// **Why not add the types package.** The page talks to WebGPU through a handful of members -- `navigator.gpu`,
// `requestAdapter`, `requestDevice`, `getContext("webgpu")`, a texture view, and the Emscripten port's handle
// table. `@webgpu/types` would be a third dev dependency in `docs/specs/dependencies.md` and
// `THIRD_PARTY_NOTICES.md` for a few dozen lines, and the repository's rule is that a new dependency is a
// decision, not a convenience (AGENTS.md non-negotiable 10). The declarations below are the members this page
// actually calls, with the spec's names, so a wrong call is still a type error.
//
// They are `declare global` because WebGPU augments platform globals; each is optional-safe at the call sites
// (`navigator.gpu` is checked before use), which is what makes a browser without WebGPU a named state rather
// than a crash.

export {};

declare global {
  interface GPUDeviceLostInfo {
    reason?: string;
    message: string;
  }

  interface GPUQueue {
    readonly label: string;
  }

  interface GPUTextureView {
    readonly label: string;
  }

  interface GPUTexture {
    createView(descriptor?: { label?: string }): GPUTextureView;
  }

  interface GPUCanvasContext {
    configure(configuration: {
      device: GPUDevice;
      format: string;
      alphaMode?: "opaque" | "premultiplied";
    }): void;
    getCurrentTexture(): GPUTexture;
  }

  interface GPUDevice {
    readonly queue: GPUQueue;
    readonly lost: Promise<GPUDeviceLostInfo>;
    readonly label: string;
    destroy(): void;
    addEventListener(type: "uncapturederror", listener: (event: GPUUncapturedErrorEvent) => void): void;
  }

  interface GPUUncapturedErrorEvent {
    readonly error?: { readonly message: string };
  }

  interface GPUAdapter {
    readonly limits: { readonly maxTextureDimension2D: number; readonly maxBufferSize: number };
    requestDevice(descriptor?: { label?: string }): Promise<GPUDevice>;
  }

  interface GPU {
    requestAdapter(): Promise<GPUAdapter | null>;
  }

  interface Navigator {
    readonly gpu?: GPU;
  }
}

declare module "*.css";
