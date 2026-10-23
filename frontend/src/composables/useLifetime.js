import { getCurrentScope, onScopeDispose } from 'vue';

// Resolve outstanding waits when a view/app is disposed. Jobs keep running on
// the phone; only the UI observer stops. No timers or polling survive unmount.
export function useLifetime() {
  let active = true;
  const waits = new Map();
  if (getCurrentScope()) onScopeDispose(() => {
    active = false;
    for (const [timer, resolve] of waits) { clearTimeout(timer); resolve(); }
    waits.clear();
  });
  return {
    get active() { return active; },
    wait(ms) { return new Promise(resolve => { const timer = setTimeout(() => { waits.delete(timer); resolve(); }, ms); waits.set(timer, resolve); }); },
  };
}
