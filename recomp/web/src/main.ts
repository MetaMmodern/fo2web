// The player-facing page (web/index.html): the start screen and the game.
import './app/style.css';
import { boot } from './app/shell';
import { prepareLocalInspector } from './gpu/local-inspector';

async function start(): Promise<void> {
  if (new URLSearchParams(location.search).get('gpuInspector') === '1') {
    try { await prepareLocalInspector(); }
    catch (e) { console.error('[gpu-inspector] could not start:', e); }
  }
  await boot();
}

void start();
