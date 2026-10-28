#!/usr/bin/env node
// One-shot visual check for the FlatOut browser port. Starts the game, waits
// for its canvas to settle, and saves both the page and canvas for inspection.
import { chromium } from '../recomp/web/node_modules/playwright/index.mjs';
import { mkdir, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const out = path.join(root, 'build', 'capture');
const url = process.env.FO2_SMOKE_URL || 'http://localhost:8080/';
await mkdir(out, { recursive: true });

const browser = await chromium.launch({
  channel: 'chrome',
  headless: false,
  args: ['--enable-unsafe-webgpu', '--use-angle=metal'],
});
try {
  const context = await browser.newContext({ viewport: { width: 1440, height: 900 }, serviceWorkers: 'block' });
  const page = await context.newPage();
  await page.goto(url, { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => {
    const button = document.querySelector('#play');
    return button instanceof HTMLButtonElement && !button.disabled;
  }, null, { timeout: 120_000 });
  await page.locator('#play').click();
  await page.waitForFunction(() => window.__nfsu2?.phase === 'playing', null, { timeout: 120_000 });
  await page.waitForTimeout(Number(process.env.FO2_CAPTURE_WAIT_MS || 20_000));
  await page.screenshot({ path: path.join(out, 'page.png') });
  const canvas = page.locator('canvas').first();
  if (await canvas.count()) await canvas.screenshot({ path: path.join(out, 'canvas.png') });
  const state = await page.evaluate(() => ({
    phase: window.__nfsu2?.phase ?? 'unknown',
    log: window.__nfsu2?.log?.snapshot?.(2 << 20) ?? '',
  }));
  await writeFile(path.join(out, 'log.txt'), state.log);
  console.log(`[capture] phase=${state.phase}; wrote ${out}/page.png and canvas.png`);
} finally {
  await browser.close();
}
