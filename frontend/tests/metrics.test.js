import test from 'node:test';
import assert from 'node:assert/strict';
import { effectScope, nextTick, ref } from 'vue';
import { useEngine } from '../src/composables/useEngine.js';
import { useLiveMetrics } from '../src/composables/useLiveMetrics.js';

const ok=value=>({code:0,stdout:JSON.stringify(value),stderr:''});
const pause=ms=>new Promise(resolve=>setTimeout(resolve,ms));
test('blocked count is current-session rule hits only; metrics polling never upgrades approximate routing state',async()=>{
  const scope=effectScope(), commands=[];
  let snapshot={running:true,iptables_active:true,scope:0,metrics:{updated_at:1,block_suffix_hits:4,block_exact_hits:3,blocked_retransmits:99,quic_rejects:100}};
  let fail=false, fullFails=false, fastOverride=null;
  const api={command:async command=>{commands.push(command);return fail || (fullFails && command==='--status') ? {code:1,stdout:'',stderr:''} : ok(command==='--status-fast' && fastOverride ? fastOverride : snapshot);},readFile:async()=>''};
  const engine=scope.run(()=>useEngine(api,{log(){},toast(){},consoleOpen:ref(false)}));
  try {
    await engine.refresh(); assert.equal(engine.blockedTotal.value,7);
    snapshot={...snapshot,metrics:{...snapshot.metrics,block_exact_hits:8}};
    await engine.refreshMetrics(); assert.equal(engine.blockedTotal.value,12);
    fail=true; await engine.refreshMetrics(); assert.equal(engine.blockedTotal.value,12); assert.equal(engine.metricsStale.value,true);
    fail=false; snapshot={...snapshot,iptables_active:false}; await engine.refresh();
    assert.equal(engine.running.value,false); fastOverride={...snapshot,iptables_active:true}; await engine.refreshMetrics();
    assert.equal(engine.running.value,false,'fast approximation must not turn an unverified route green');
    assert.equal(commands.at(-1),'--status','recovery requires full validation');
    fastOverride=null; snapshot={...snapshot,iptables_active:true,daemon_image_current:false};
    await engine.refreshMetrics(); assert.equal(engine.running.value,false); assert.equal(engine.badge.value,'升级待生效');
    snapshot={...snapshot,daemon_image_current:true}; await engine.refreshMetrics(); assert.equal(engine.running.value,true);
    fullFails=true; fastOverride={...snapshot,iptables_active:false}; await engine.refreshMetrics();
    assert.equal(engine.running.value,false,'an entry failure must clear green even if the full check fails');
    assert.equal(engine.metricsStale.value,true);
    fullFails=false; fastOverride=null;
    await engine.refreshMetrics(); assert.equal(engine.running.value,true);
    snapshot={...snapshot,iptables_active:false}; await engine.refreshMetrics();
    assert.equal(engine.running.value,false); assert.equal(engine.badge.value,'路由待恢复');
    assert.match(engine.description.value,/启动守护/);
    for(const metrics of [null,{}, {updated_at:0,block_suffix_hits:0,block_exact_hits:0},{updated_at:1,block_suffix_hits:-1,block_exact_hits:0},
      {updated_at:1,block_suffix_hits:Number.MAX_SAFE_INTEGER,block_exact_hits:1}]) {
      snapshot={...snapshot,metrics}; await engine.refresh(); assert.equal(engine.blockedTotal.value,null);
    }
    assert.ok(commands.includes('--status-fast'));
  } finally {scope.stop();}
});
test('live metrics keep observing a degraded process, stop on hidden tab/background/unmount and never overlap polling',async()=>{
  const scope=effectScope(), active=ref('home'), running=ref(true), document=new EventTarget(); document.hidden=false;
  let calls=0, concurrent=0, peak=0;
  const engine={running,state:ref({running:true}),metricsStale:ref(false),refreshMetrics:async()=>{calls++;concurrent++;peak=Math.max(peak,concurrent);await pause(2);concurrent--;}};
  scope.run(()=>useLiveMetrics(engine,active,document,10));
  await pause(25); assert.ok(calls>=1); assert.equal(peak,1);
  running.value=false; const degraded=calls; await pause(20); assert.ok(calls>degraded,'a live degraded daemon must still be observed');
  active.value='settings'; await nextTick(); await pause(5); const hidden=calls;
  await pause(20); assert.equal(calls,hidden);
  active.value='home'; await nextTick(); await pause(15); assert.ok(calls>hidden);
  document.hidden=true; document.dispatchEvent(new Event('visibilitychange')); await nextTick(); await pause(5); const background=calls;
  await pause(20); assert.equal(calls,background);
  scope.stop(); document.hidden=false; document.dispatchEvent(new Event('visibilitychange')); await pause(20); assert.equal(calls,background);
});
