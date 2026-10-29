#!/usr/bin/env node
// Summarize a WebGPU Inspector .wgpuc capture and inspect the CPU-transformed
// FlatOut TVert data used by the large car/scene draws.
import { open, stat } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const usage = () => {
  console.error('Usage: node tools/inspect-wgpuc.mjs <capture.wgpuc> [--min-vertices N] [--summary]');
  process.exit(2);
};

const args = process.argv.slice(2);
if (!args.length || args[0].startsWith('-')) usage();
const file = path.resolve(args[0]);
let minVertices = 500;
let summaryOnly = false;
for (let i = 1; i < args.length; i++) {
  if (args[i] === '--min-vertices' && args[i + 1]) minVertices = Number(args[++i]);
  else if (args[i] === '--summary') summaryOnly = true;
  else usage();
}
if (!Number.isInteger(minVertices) || minVertices < 1) usage();

const fh = await open(file, 'r');
try {
  const fileSize = (await stat(file)).size;
  const lineBuf = Buffer.alloc(128);
  const { bytesRead: lineLength } = await fh.read(lineBuf, 0, lineBuf.length, 0);
  const lineEnd = lineBuf.indexOf(10, 0);
  if (lineEnd < 0 || lineEnd >= lineLength) throw new Error('Missing WGPUCAP header line');
  const header = lineBuf.toString('utf8', 0, lineEnd).trim().split(/\s+/);
  if (header[0] !== 'WGPUCAP' || header.length < 3) throw new Error('Unsupported capture header');
  const metadataLength = Number(header[2]);
  if (!Number.isSafeInteger(metadataLength) || metadataLength < 2 || metadataLength > fileSize) {
    throw new Error(`Invalid metadata length: ${header[2]}`);
  }
  const metadataBuf = Buffer.alloc(metadataLength);
  const { bytesRead } = await fh.read(metadataBuf, 0, metadataLength, lineEnd + 1);
  if (bytesRead !== metadataLength) throw new Error('Truncated capture metadata');
  const data = JSON.parse(metadataBuf.toString('utf8'));
  const payloadBase = (lineEnd + 1 + metadataLength + 7) & ~7;
  const objects = data.objects ?? {};
  const commands = data.commands ?? [];
  const payloads = data.payloadTable ?? [];
  const label = (id, cls) => {
    const obj = objects[String(id)];
    if (!obj) return `${cls ?? 'object'}#${id}`;
    return `${obj.label ?? obj.descriptor?.label ?? obj.type}#${id}`;
  };

  const bufCandidates = [];
  for (const c of commands) {
    for (const b of c.bufferData ?? []) {
      if (Number(b.byteLength) > 1024 * 1024 && Number.isInteger(b.__payloadId)) {
        bufCandidates.push({ command: c, entry: b });
      }
    }
  }
  bufCandidates.sort((a, b) => b.entry.byteLength - a.entry.byteLength);
  const vertexPayload = bufCandidates[0];
  let vertices = null;
  if (vertexPayload) {
    const [rel, length] = payloads[vertexPayload.entry.__payloadId] ?? [];
    if (Number.isInteger(rel) && Number.isInteger(length) && rel >= 0 && length >= 0 && payloadBase + rel + length <= fileSize) {
      vertices = Buffer.alloc(length);
      const got = await fh.read(vertices, 0, length, payloadBase + rel);
      if (got.bytesRead !== length) vertices = null;
    }
  }

  const stats = data.statistics ?? {};
  console.log(`Capture: ${file}`);
  console.log(`Format ${header[1]}, tool ${data.tool ?? '?'} ${data.toolVersion ?? ''}, frame ${data.frame ?? '?'}`);
  console.log(`${(fileSize / 1048576).toFixed(1)} MiB; ${Object.keys(objects).length} objects; ${commands.length} commands; ${payloads.length} payloads`);
  if (Object.keys(stats).length) console.log(`Statistics: ${JSON.stringify(stats)}`);
  if (vertexPayload && vertices) {
    const groupId = vertexPayload.command.args?.[1]?.__id;
    const group = objects[String(groupId)];
    const bufferEntry = group?.descriptor?.entries?.[vertexPayload.entry.entryIndex];
    const bufferId = bufferEntry?.resource?.buffer?.__id;
    console.log(`Largest vertex-like buffer: ${label(bufferId, 'buffer')} ${vertices.length.toLocaleString()} bytes (payload ${vertexPayload.entry.__payloadId})`);
  } else {
    console.log('No captured buffer over 1 MiB found.');
  }

  const boundGroups = new Map();
  let pipeline = null;
  const currentTextureLabels = () => {
    const result = [];
    for (const [slot, id] of [...boundGroups].sort((a, b) => a[0] - b[0])) {
      const group = objects[String(id)];
      for (let binding = 0; binding < (group?.descriptor?.entries?.length ?? 0); binding++) {
        const e = group.descriptor.entries[binding];
        const viewId = e.resource?.__id;
        const view = objects[String(viewId)];
        const tex = view?.texture?.__id;
        if (tex != null) {
          const t = objects[String(tex)];
          const dims = t ? `${t.width ?? '?'}x${t.height ?? '?'}:${t.format ?? '?'}` : '';
          result.push(`${slot}.${binding}:${label(tex, 'texture')}(${dims})`);
        }
      }
    }
    return [...new Set(result)].join(',');
  };
  const drawRows = [];
  for (const c of commands) {
    if (c.method === 'setPipeline') pipeline = c.args?.[0]?.__id ?? null;
    else if (c.method === 'setBindGroup') boundGroups.set(c.args?.[0], c.args?.[1]?.__id);
    else if (c.method === 'draw') {
      const [count = 0, instances = 1, first = 0] = c.args ?? [];
      const p = objects[String(pipeline)];
      const shaderId = p?.descriptor?.fragment?.module?.__id;
      const shader = objects[String(shaderId)];
      drawRows.push({
        command: c.index,
        first: Number(first),
        count: Number(count),
        instances: Number(instances),
        pipeline: p?.label ?? p?.descriptor?.label ?? `pipeline#${pipeline}`,
        shader: shader?.label ?? `shader#${shaderId}`,
        textures: currentTextureLabels(),
      });
    }
  }
  console.log(`Draw calls: ${drawRows.length}`);
  if (summaryOnly) {
    const groups = new Map();
    for (const d of drawRows) {
      const key = `${d.shader} | ${d.textures}`;
      const g = groups.get(key) ?? { calls: 0, vertices: 0, max: 0 };
      g.calls++; g.vertices += d.count; g.max = Math.max(g.max, d.count); groups.set(key, g);
    }
    for (const [key, g] of groups) console.log(`${key} | calls=${g.calls} vertices=${g.vertices} maxDraw=${g.max}`);
  }
  if (!summaryOnly) for (const d of drawRows) {
    if (d.count < minVertices) continue;
    let coords = '';
    if (vertices && (d.first + d.count) * 112 <= vertices.length) {
      const names = [['pos', 0, 4], ['color0', 4, 4], ['color1', 8, 4], ['uv0', 12, 4], ['uv1', 16, 4], ['uv2', 20, 4], ['uv3', 24, 4]];
      const ranges = [];
      for (const [name, word, n] of names) {
        const lo = Array(n).fill(Infinity), hi = Array(n).fill(-Infinity);
        for (let vi = d.first; vi < d.first + d.count; vi++) {
          const off = vi * 112 + word * 4;
          for (let k = 0; k < n; k++) {
            const x = vertices.readFloatLE(off + k * 4);
            if (!Number.isFinite(x)) continue;
            lo[k] = Math.min(lo[k], x); hi[k] = Math.max(hi[k], x);
          }
        }
        ranges.push(`${name}=[${lo.map((x, k) => `${x.toFixed(2)}..${hi[k].toFixed(2)}`).join(',')}]`);
      }
      coords = ` ${ranges.join(' ')}`;
    }
    console.log(`#${d.command} ${d.shader} first=${d.first} count=${d.count} ${d.textures}${coords}`);
  }
} finally {
  await fh.close();
}
