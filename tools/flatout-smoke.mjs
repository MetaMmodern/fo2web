#!/usr/bin/env node
// Keep one local Chrome session running FlatOut. After each game exit, save
// window.__nfsu2.log and wait for a new wasm build, then reload and press Play.
// This replaces the manual launch / Download log cycle during runtime triage.
import { chromium } from '../recomp/web/node_modules/playwright/index.mjs';
import { appendFile, mkdir, readdir, stat, unlink, writeFile } from 'node:fs/promises';
import { spawn } from 'node:child_process';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const wasmPath = path.join(root, 'recomp/out/web/nfsu2.wasm');
const outDir = path.join(root, 'build');
const logPath = path.join(outDir, 'flatout-smoke.log');
const livePath = path.join(outDir, 'flatout-smoke-live.log');
const browserLogPath = path.join(outDir, 'flatout-smoke-browser.log');
const screenPrefix = 'flatout-smoke-screen-';
const baseUrl = process.env.FO2_SMOKE_URL || 'http://localhost:8080/';
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const wasmStamp = async () => (await stat(wasmPath)).mtimeMs;

async function canReachServer() {
  try { return (await fetch(baseUrl, { signal: AbortSignal.timeout(1500) })).ok; }
  catch { return false; }
}

let localServer = null;
if (!(await canReachServer())) {
  console.log('[smoke] local server is down; starting recomp/web/start-flatout.sh');
  localServer = spawn(path.join(root, 'recomp/web/start-flatout.sh'), [], { cwd: root, stdio: 'inherit' });
  localServer.on('exit', code => console.error(`[smoke] local server exited (${code})`));
  for (let i = 0; i < 60 && !(await canReachServer()); i++) await wait(1000);
  if (!(await canReachServer())) throw new Error('FlatOut server did not start on localhost:8080');
}

await mkdir(outDir, { recursive: true });
const browser = await chromium.launch({
  channel: 'chrome',
  headless: false,
  args: ['--enable-unsafe-webgpu', '--use-angle=metal'],
});
const context = await browser.newContext({
  viewport: { width: 1440, height: 900 },
  serviceWorkers: 'block',
});
const page = await context.newPage();
await writeFile(browserLogPath, `===== ${new Date().toISOString()} browser diagnostics =====\n`);
const saveBrowserError = async (kind, detail) => {
  const line = `[${new Date().toISOString()}] ${kind}: ${detail}\n`;
  console.error(line.trimEnd());
  try { await appendFile(browserLogPath, line); }
  catch (error) { console.error(`[smoke] could not persist browser error: ${error}`); }
};
// Playwright's Page API has no setCacheEnabled method. For Chromium, disable
// cache through the DevTools protocol so each reload picks up the rebuilt UI.
const cdp = await context.newCDPSession(page);
await cdp.send('Network.enable');
await cdp.send('Network.setCacheDisabled', { cacheDisabled: true });
page.on('dialog', async dialog => {
  if (dialog.type() === 'beforeunload') await dialog.accept();
  else console.log(`[browser] dialog (${dialog.type()}): ${dialog.message()}`);
});
page.on('pageerror', error => { void saveBrowserError('pageerror', error.stack || error); });
page.on('console', message => {
  if (message.type() === 'error') void saveBrowserError('console', message.text());
});
page.on('crash', () => { void saveBrowserError('page-crash', 'Chromium renderer crashed'); });

let stamp = await wasmStamp();
let run = 0;
try {
  await page.goto(baseUrl, { waitUntil: 'domcontentloaded' });
  for (;;) {
    run++;
    console.log(`[smoke] run ${run}: waiting for Play to be enabled at ${baseUrl}`);
    await page.waitForFunction(() => {
      const b = document.querySelector('#play');
      return b instanceof HTMLButtonElement && !b.disabled;
    }, null, { timeout: 120000 });
    await page.locator('#play').click();
    let phase = '';
    let log = '';
    let reloadedForBuild = false;
    let lastScreenCapture = 0;
    while (phase !== 'exited') {
      await wait(5000);
      const currentStamp = await wasmStamp();
      if (currentStamp !== stamp) {
        console.log(`[smoke] run ${run}: WASM changed while game was active; reloading into the new build`);
        stamp = currentStamp;
        await page.reload({ waitUntil: 'domcontentloaded' });
        reloadedForBuild = true;
        break;
      }
      ({ phase, log } = await page.evaluate(() => ({
        phase: window.__nfsu2?.phase ?? 'page-loading',
        log: window.__nfsu2?.log?.snapshot?.(32 << 20) ?? '(no in-page game log available)',
      })));
      if (phase === 'playing' && Date.now() - lastScreenCapture >= 15000) {
        try {
          const stamp = new Date().toISOString().replace(/[:.]/g, '-');
          const screenPath = path.join(outDir, `${screenPrefix}${stamp}.png`);
          const renderState = await page.evaluate(() => {
            const s = window.__nfsu2?.session;
            return { stats: s?.renderer?.stats?.() ?? null, view: s?.viewRect?.() ?? null };
          });
          await page.screenshot({ path: screenPath, animations: 'disabled' });
          lastScreenCapture = Date.now();
          await appendFile(browserLogPath, `[${new Date().toISOString()}] screenshot: ${screenPath}; renderer=${JSON.stringify(renderState)}\n`);
          const captures = (await readdir(outDir)).filter(n => n.startsWith(screenPrefix) && n.endsWith('.png')).sort();
          for (const old of captures.slice(0, Math.max(0, captures.length - 20))) await unlink(path.join(outDir, old));
        } catch (error) { await saveBrowserError('screenshot', error); }
      }
      const stampText = `===== ${new Date().toISOString()} run ${run} phase=${phase} =====\n`;
      await writeFile(livePath, stampText + log);
      if (phase !== 'exited') console.log(`[smoke] run ${run}: phase=${phase}; live log updated`);
    }
    if (reloadedForBuild) continue;

    const rev = log.match(/game module ([^:]+):/)?.[1] ?? 'unknown';
    const entry = `\n\n===== ${new Date().toISOString()} run ${run} rev=${rev} =====\n${log}\n`;
    await appendFile(logPath, entry);
    console.log(`[smoke] run ${run} exited; full log appended to ${logPath}`);
    console.log('[smoke] waiting for a new recomp build; the page will reload and retry automatically');

    while ((await wasmStamp()) === stamp) await wait(1000);
    stamp = await wasmStamp();
    await page.reload({ waitUntil: 'domcontentloaded' });
  }
} finally {
  await browser.close();
  if (localServer && localServer.exitCode === null) localServer.kill('SIGTERM');
}
