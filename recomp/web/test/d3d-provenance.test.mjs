import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';

test('capture exports CPU D3D provenance with stable draw correlation', () => {
  const root = new URL('../../../', import.meta.url);
  const gpu = readFileSync(new URL('recomp/runtime/web/gpu_proto.h', root), 'utf8');
  const com = readFileSync(new URL('recomp/runtime/com.c', root), 'utf8');
  const encoder = readFileSync(new URL('recomp/web/src/gpu/encoder.ts', root), 'utf8');
  const page = readFileSync(new URL('recomp/web/src/gpu/index.ts', root), 'utf8');

  assert.match(gpu, /typedef struct GwD3DDraw/);
  assert.match(com, /metal_set_draw_provenance\(&p, g_mem \+ vp\)/);
  assert.match(com, /g_cur_tex == 14u.*g_cur_ps_id == 14/s);
  assert.match(com, /g_cur_tex == 83u.*g_cur_ps_id == 28.*g_cur_ps_id == 29/s);
  assert.match(encoder, /correlationMarker: marker/);
  assert.match(encoder, /generatedTVertBytes:/);
  assert.match(page, /stream\.metadata\.d3dDraws = st\.d3dDraws/);
  assert.match(page, /d\.webgpuCommandIndex = byMarker/);
});
