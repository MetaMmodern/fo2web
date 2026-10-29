/** Optional, same-origin WebGPU Inspector instrumentation for local frame captures. */
export interface LocalGpuInspector {
  initialize(): void;
  beginFrameCapture(options?: { maxBufferSize?: number }): void;
  endFrameCapture(): void;
  saveCaptureData(filename?: string): Promise<unknown>;
}

declare global {
  interface Window {
    webgpuInspector?: LocalGpuInspector;
    fo2CaptureGpuFrame?: () => Promise<void>;
  }
}

export async function prepareLocalInspector(): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const script = document.createElement('script');
    script.src = '/webgpu-inspector.js';
    script.onload = () => resolve();
    script.onerror = () => reject(new Error('Could not load /webgpu-inspector.js'));
    document.head.appendChild(script);
  });
  if (!window.webgpuInspector) throw new Error('WebGPU Inspector loaded without installing its page API');
  // Must run before boot() can request a GPU adapter or device.
  window.webgpuInspector.initialize();
  console.info('[gpu-inspector] ready; after Play, call fo2CaptureGpuFrame() in DevTools to save one real game frame');
}
