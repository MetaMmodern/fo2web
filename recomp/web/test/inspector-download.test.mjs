import { readFileSync } from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';

test('inspector keeps capture download URL alive for ten minutes', () => {
  const source = readFileSync(new URL('../src/static/webgpu-inspector.js', import.meta.url), 'utf8');
  const start = source.indexOf('_downloadCaptureStream(e,t){');
  const end = source.indexOf('async readBuffer(e,t,r){', start);
  assert.ok(start >= 0 && end > start);
  const method = source.slice(start, end).replace('_downloadCaptureStream(e,t){', 'function(e,t){');
  const events = [], timers = [];
  const anchor = { click() { events.push(['click', this.href, this.download]); } };
  const document = {
    createElement(tag) { events.push(['createElement', tag]); return anchor; },
    body: {
      appendChild(node) { events.push(['appendChild', node]); },
      removeChild(node) { events.push(['removeChild', node]); },
    },
  };
  const blob = {};
  const url = {
    createObjectURL(value) { assert.equal(value, blob); return 'blob:capture'; },
    revokeObjectURL(value) { events.push(['revoke', value]); },
  };
  const run = Function('s', 'URL', 'setTimeout', `return (${method})`)(
    document, url, (callback, delay) => timers.push({ callback, delay }),
  );
  const stream = {};
  run.call({ captureStreamToBlob(value) { assert.equal(value, stream); return blob; } }, stream, 'capture.wgpuc');

  assert.deepEqual(events, [
    ['createElement', 'a'],
    ['appendChild', anchor],
    ['click', 'blob:capture', 'capture.wgpuc'],
    ['removeChild', anchor],
  ]);
  assert.equal(timers.length, 1);
  assert.equal(timers[0].delay, 10 * 60 * 1000);
  timers[0].callback();
  assert.deepEqual(events.at(-1), ['revoke', 'blob:capture']);
});
