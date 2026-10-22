import { onScopeDispose, ref, watch } from 'vue';
import { useLifetime } from './useLifetime.js';

// Poll only the visible overview, not hidden tabs or backgrounded WebViews.
export function useLiveMetrics(engine, activePage, document = globalThis.document, interval = 5000) {
  const life=useLifetime(), visible=ref(!document?.hidden);
  const visibility=()=>{visible.value=!document.hidden;};
  document?.addEventListener('visibilitychange',visibility);
  onScopeDispose(()=>document?.removeEventListener('visibilitychange',visibility));
  watch(()=>engine.running.value && activePage.value==='home' && visible.value, async (enabled,_,cleanup)=>{
    if (!enabled) return;
    let cancelled=false, timer, wake;
    cleanup(()=>{cancelled=true;clearTimeout(timer);wake?.();});
    while (life.active && !cancelled) {
      try { await engine.refreshMetrics(); } catch { if(life.active) engine.metricsStale.value=true; }
      if (!life.active || cancelled) break;
      await new Promise(resolve=>{wake=resolve;timer=setTimeout(resolve,interval);});
    }
  },{immediate:true});
}
