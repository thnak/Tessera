// SPDX-License-Identifier: MIT
// W0 spike (throwaway): stand-in for the lazy `tessera_canvas` module. It is imported only on the first
// onVisibility(true) (layout spec 4.2), owns a real WebGPU context and cancels its frame loop when hidden.

export interface CanvasSession {
  visible(on: boolean): void;
  dispose(): void;
}

export interface SpikeStats {
  frames: number;
  importedAt: number | null;
  contextCreations: number;
  gpu: boolean;
}

export async function startCanvas(canvas: HTMLCanvasElement, stats: SpikeStats): Promise<CanvasSession> {
  let raf = 0;
  let device: GPUDevice | null = null;
  let context: GPUCanvasContext | null = null;
  const adapter = navigator.gpu ? await navigator.gpu.requestAdapter() : null;
  stats.gpu = adapter !== null;
  if (adapter) {
    device = await adapter.requestDevice();
    context = canvas.getContext("webgpu");
    stats.contextCreations += 1;
    context?.configure({ device, format: navigator.gpu.getPreferredCanvasFormat(), alphaMode: "opaque" });
  }
  const draw = (): void => {
    stats.frames += 1;
    if (device && context) {
      const encoder = device.createCommandEncoder();
      const pass = encoder.beginRenderPass({
        colorAttachments: [
          {
            view: context.getCurrentTexture().createView(),
            clearValue: { r: (stats.frames % 100) / 100, g: 0.3, b: 0.6, a: 1 },
            loadOp: "clear",
            storeOp: "store",
          },
        ],
      });
      pass.end();
      device.queue.submit([encoder.finish()]);
    }
    raf = requestAnimationFrame(draw);
  };
  return {
    visible(on) {
      cancelAnimationFrame(raf);
      raf = 0;
      if (on) raf = requestAnimationFrame(draw);
    },
    dispose() {
      cancelAnimationFrame(raf);
      raf = 0;
      device?.destroy();
    },
  };
}
