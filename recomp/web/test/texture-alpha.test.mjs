// Synthetic recorder + mock WebGPU objects. No browser, game, or GPU session.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import * as esbuild from 'esbuild';
import { fileURLToPath } from 'node:url';

const js = (await esbuild.build({
  stdin: {
    contents: `export { Encoder } from './src/gpu/encoder';
      export { opaqueTextureMask } from './src/gpu/shader-abi';
      export * as P from './src/gpu/proto';`,
    resolveDir: fileURLToPath(new URL('../', import.meta.url)),
  },
  bundle: true, format: 'esm', write: false,
})).outputFiles[0].text;
const { Encoder, opaqueTextureMask, P } = await import('data:text/javascript;base64,' + Buffer.from(js).toString('base64'));

test('only X8 bindings force alpha, independently on all four stages', () => {
  for (let mask = 0; mask < 16; mask++) {
    const formats = [0, 1, 2, 3].map(k => mask & (1 << k) ? P.Fmt.X8R8G8B8 : P.Fmt.A8R8G8B8);
    assert.equal(opaqueTextureMask(...formats), mask);
  }
  assert.equal(opaqueTextureMask(P.Fmt.A8, P.Fmt.DXT1, P.Fmt.DXT3, P.Fmt.DXT5), 0);
});

function setup() {
  globalThis.GPUShaderStage = { VERTEX: 1, FRAGMENT: 2 };
  globalThis.GPUBufferUsage = { STORAGE: 1, COPY_DST: 2, UNIFORM: 4, INDEX: 8, COPY_SRC: 16 };
  globalThis.GPUTextureUsage = { TEXTURE_BINDING: 1, COPY_DST: 2, COPY_SRC: 4, RENDER_ATTACHMENT: 8 };
  const texture = d => ({
    ...d, width: d.size[0], height: d.size[1], depthOrArrayLayers: d.size[2] ?? 1,
    createView: () => ({}), destroy() {},
  });
  const state = { uniforms: null, pipelines: [], offsets: [], copies: 0 };
  const device = {
    features: new Set(), // Must also work without texture-component-swizzle.
    queue: {
      writeTexture() {},
      writeBuffer(buf, offset, data, start, count) {
        if (buf.label === 'pspar') state.uniforms = data.slice(start, start + count);
      },
    },
    createTexture: texture,
    createBuffer: d => ({ ...d, destroy() {} }),
    createShaderModule: d => ({ ...d, getCompilationInfo: async () => ({ messages: [] }) }),
    createBindGroupLayout: d => d, createPipelineLayout: d => d,
    createBindGroup: d => d, createSampler: d => d,
    createRenderPipeline: d => { state.pipelines.push(d); return d; },
    pushErrorScope() {}, popErrorScope: async () => null,
  };
  const pass = {
    setPipeline() {}, setBlendConstant() {}, setScissorRect() {}, setViewport() {},
    setBindGroup(slot, group, offsets) { if (slot === 0 && offsets) state.offsets.push(offsets[1]); },
    draw() {}, end() {},
  };
  const command = { beginRenderPass: () => pass, copyTextureToTexture() { state.copies++; } };
  const memory = new WebAssembly.Memory({ initial: 16 });
  const u = new Uint32Array(memory.buffer);
  const encoder = new Encoder(device, memory, 0, () => {});
  const fw = P.C.frame, draws = 4096, constants = 8192;
  u[fw + P.F.draws] = draws * 4; u[fw + P.F.psc] = constants * 4;
  u[fw + P.F.bbW] = 256; u[fw + P.F.bbH] = 256;
  return { encoder, u, fw, draws, command, state, direct: texture({ size: [256, 256], format: 'bgra8unorm' }) };
}

test('PS uniform reuse includes resolved texture format, not just shader/constants', () => {
  const s = setup();
  s.encoder.texCreate(1, 256, 256, 1, P.Fmt.A8R8G8B8, 0);
  s.encoder.texCreate(2, 256, 256, 1, P.Fmt.X8R8G8B8, 0);
  // The final missing texture resolves to white; its stale requested ID must
  // not leak X8 state from the preceding draw.
  const textures = [1, 2, 2, 999];
  s.u[s.fw + P.F.dn] = textures.length;
  textures.forEach((tex, i) => {
    const w = s.draws + i * P.DRAW_WORDS;
    s.u[w + P.D.tex] = tex; s.u[w + P.D.count] = 3;
  });
  const stats = s.encoder.encodeFrame(0, s.command, s.direct);
  assert.equal(stats.draws, 4);
  assert.deepEqual(s.state.offsets, [0, 768, 1536]);
  assert.equal(s.state.uniforms[2], 0);
  assert.equal(s.state.uniforms[768 / 4 + 2], 1);
  assert.equal(s.state.uniforms[1536 / 4 + 2], 0);
});

test('X8 to A8 StretchRect materializes opaque alpha instead of copying unused bytes', () => {
  const s = setup();
  s.encoder.texCreate(1, 256, 256, 1, P.Fmt.X8R8G8B8, 0);
  s.encoder.texCreate(2, 256, 256, 1, P.Fmt.A8R8G8B8, P.TF_RT);
  s.u[s.fw + P.F.dn] = 1;
  s.u[s.draws + P.D.kind] = P.Kind.Blit;
  s.u[s.draws + P.D.blitSrc] = 1; s.u[s.draws + P.D.target] = 2;
  s.encoder.encodeFrame(0, s.command, s.direct);
  assert.equal(s.state.copies, 0);
  assert.ok(s.state.pipelines.some(p => p.fragment.entryPoint === 'fs_opaque'));
});

test('A8 to A8 StretchRect retains its alpha-preserving raw-copy path', () => {
  const s = setup();
  s.encoder.texCreate(1, 256, 256, 1, P.Fmt.A8R8G8B8, 0);
  s.encoder.texCreate(2, 256, 256, 1, P.Fmt.A8R8G8B8, P.TF_RT);
  s.u[s.fw + P.F.dn] = 1;
  s.u[s.draws + P.D.kind] = P.Kind.Blit;
  s.u[s.draws + P.D.blitSrc] = 1; s.u[s.draws + P.D.target] = 2;
  s.encoder.encodeFrame(0, s.command, s.direct);
  assert.equal(s.state.copies, 1);
});
