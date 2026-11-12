// The player-facing page (web/index.html): the start screen and the game.
import './app/style.css';
import { boot } from './app/shell';
import { prepareLocalInspector } from './gpu/local-inspector';

/** Place the existing, stateful page panels into the launcher's native dialogs before
 * boot wires them up.  Moving nodes (rather than copying markup) keeps every ID,
 * event listener, and runtime-owned panel singular. */
function mountLauncherPanels(): void {
  const move = (panel: string, target = panel) => {
    const source = document.getElementById(panel);
    const slot = document.querySelector<HTMLElement>(`[data-dialog-content="${target}"]`);
    if (source && slot) slot.append(source);
  };
  move('controls');
  move('game-data');
  move('changes', 'about');
  move('about');
  // Dialog headers are visible; retain the original headings for semantics only.
  for (const id of ['controls-title', 'game-data-title', 'changes-title', 'about-title']) {
    document.getElementById(id)?.setAttribute('hidden', '');
  }
  const runtimeNotes = document.getElementById('about-runtime-notes');
  if (runtimeNotes) {
    for (const id of ['log-btn', 'lastgame', 'report', 'newer']) {
      const item = document.getElementById(id);
      if (item) runtimeNotes.append(item);
    }
  }

  for (const dialog of document.querySelectorAll<HTMLDialogElement>('dialog:not(.confirm)')) {
    dialog.addEventListener('click', event => {
      if (event.target === dialog) dialog.close('cancel');
    });
  }

  document.addEventListener('click', event => {
    const trigger = (event.target as Element | null)?.closest<HTMLElement>('[data-panel]');
    if (!trigger) return;
    const dialog = document.getElementById(`dialog-${trigger.dataset.panel}`);
    if (!(dialog instanceof HTMLDialogElement) || dialog.open) return;
    event.preventDefault();
    dialog.showModal();
    dialog.querySelector<HTMLElement>('button, [tabindex], input, select')?.focus();
  });
}

async function start(): Promise<void> {
  mountLauncherPanels();
  if (new URLSearchParams(location.search).get('gpuInspector') === '1') {
    try { await prepareLocalInspector(); }
    catch (e) { console.error('[gpu-inspector] could not start:', e); }
  }
  await boot();
}

void start();
