import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';

test('inspector bounds all captured buffer payloads by maxCaptureBytes', () => {
  const source = readFileSync(new URL('../src/static/webgpu-inspector.js', import.meta.url), 'utf8');
  const start = source.indexOf('_queueCaptureBuffer(e,t,r,s,a,n){');
  const end = source.indexOf('_recordCaptureBuffers(e,t){', start);
  assert.ok(start >= 0 && end > start);
  const method = source.slice(start, end).replace('_queueCaptureBuffer(e,t,r,s,a,n){', 'function(e,t,r,s,a,n){');
  const run = Function(`return (${method})`)();
  const inspector = {
    _captureMaxBufferSize: 1024,
    _captureBytesRemaining: 10,
    _captureBuffersCount: 0,
    _matchesCaptureScope: () => true,
  };
  const encoder = {};
  const buffer = {};
  run.call(inspector, encoder, 1, 2, buffer, 0, 20);
  run.call(inspector, encoder, 2, 3, buffer, 0, 20);

  assert.equal(inspector._captureBytesRemaining, 0);
  assert.equal(inspector._captureBuffersCount, 1);
  assert.deepEqual(encoder.__captureBuffers, [{
    commandId: 1, entryIndex: 2, buffer, offset: 0, size: 10, originalSize: 20,
  }]);
  assert.match(source, /_captureBytesRemaining='number'==typeof e\.maxCaptureBytes/);
  assert.match(source, /if\(v>=0&&_>v\)return;v>=0&&\(this\._captureBytesRemaining-=_\)/);
});
