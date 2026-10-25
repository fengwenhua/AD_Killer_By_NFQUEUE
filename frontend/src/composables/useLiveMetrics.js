import { onScopeDispose, ref, watch } from 'vue';
import { useLifetime } from './useLifetime.js';

// Poll only the visible overview, not hidden tabs or backgrounded WebViews.
export function useLiveMetrics(engine, activePage, document = globalThis.document, interval = 5000) {
  const life=useLifetime(), visible=ref(!document?.hidden);
  const visibility=()=>{visible.value=!document.hidden;};
  document?.addEventListener('visibilitychange',visibility);
  onScopeDispose(()=>document?.removeEventListener('visibilitychange',visibility));
  // Keep watching a live but degraded daemon, so an external/manual recovery
  // can become visible without reopening the WebUI.
  watch(()=>(engine.state ? !!engine.state.value?.running : engine.running.value) && activePage.value==='home' && visible.value, async (enabled,_,cleanup)=>{
    if (!enabled) return;
    let cancelled=false, timer, wake;
    const ageTimer=setInterval(()=>engine.tickMetrics?.(),1000);
    cleanup(()=>{cancelled=true;clearTimeout(timer);clearInterval(ageTimer);wake?.();});
    while (life.active && !cancelled) {
      engine.tickMetrics?.();
      try { await engine.refreshMetrics(); } catch { if(life.active) engine.metricsStale.value=true; }
      if (!life.active || cancelled) break;
      await new Promise(resolve=>{wake=resolve;timer=setTimeout(resolve,interval);});
    }
  },{immediate:true});
}
