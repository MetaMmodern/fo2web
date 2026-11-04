import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';

test('inspector splits capture reads into bounded pools without dropping entries', () => {
  const source = readFileSync(new URL('../src/static/webgpu-inspector.js', import.meta.url), 'utf8');
  const start = source.indexOf('_recordCaptureBuffers(e,t){');
  const end = source.indexOf('_isCompatibilityMode(e){', start);
  assert.ok(start >= 0 && end > start);
  const method = source.slice(start, end).replace('_recordCaptureBuffers(e,t){', 'function(e,t){');
  const run = Function('GPUBufferUsage', `return (${method})`)({ COPY_DST: 1, MAP_READ: 2 });
  const pools = [], copies = [];
  const device = { limits: { maxBufferSize: 2 * 1024 ** 3 }, createBuffer: d => (pools.push(d.size), {}) };
  const encoder = { __device: device, copyBufferToBuffer: (...args) => copies.push(args) };
  const reads = Array.from({ length: 240 }, (_, i) => ({ commandId: i, entryIndex: 0, buffer: {}, offset: 0, size: 9 * 1024 ** 2 }));
  const inspector = { _captureBuffersCount: reads.length, _captureTempBuffers: [], disableRecording() {}, enableRecording() {} };
  run.call(inspector, encoder, reads);
  assert.equal(copies.length, reads.length);
  assert.equal(inspector._captureBuffersCount, 0);
  assert.ok(pools.length > 1);
  assert.ok(pools.every(size => size <= 128 * 1024 ** 2));
  assert.equal(inspector._captureTempBuffers.flatMap(p => p.ranges).length, reads.length);
});
