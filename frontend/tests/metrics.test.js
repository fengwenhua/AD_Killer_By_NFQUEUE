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
  let fail=false;
  const api={command:async command=>{commands.push(command);return fail ? {code:1,stdout:'',stderr:''} : ok(snapshot);},readFile:async()=>''};
  const engine=scope.run(()=>useEngine(api,{log(){},toast(){},consoleOpen:ref(false)}));
  try {
    await engine.refresh(); assert.equal(engine.blockedTotal.value,7);
    snapshot={...snapshot,metrics:{...snapshot.metrics,block_exact_hits:8}};
    await engine.refreshMetrics(); assert.equal(engine.blockedTotal.value,12);
    fail=true; await engine.refreshMetrics(); assert.equal(engine.blockedTotal.value,12); assert.equal(engine.metricsStale.value,true);
    fail=false; snapshot={...snapshot,iptables_active:false}; await engine.refresh();
    assert.equal(engine.running.value,false); snapshot={...snapshot,iptables_active:true}; await engine.refreshMetrics();
    assert.equal(engine.running.value,false,'fast approximation must not turn an unverified route green');
    for(const metrics of [null,{}, {updated_at:0,block_suffix_hits:0,block_exact_hits:0},{updated_at:1,block_suffix_hits:-1,block_exact_hits:0},
      {updated_at:1,block_suffix_hits:Number.MAX_SAFE_INTEGER,block_exact_hits:1}]) {
      snapshot={...snapshot,metrics}; await engine.refresh(); assert.equal(engine.blockedTotal.value,null);
    }
    assert.ok(commands.includes('--status-fast'));
  } finally {scope.stop();}
});
test('live metrics stop on hidden tab/background/unmount and never overlap polling',async()=>{
  const scope=effectScope(), active=ref('home'), running=ref(true), document=new EventTarget(); document.hidden=false;
  let calls=0, concurrent=0, peak=0;
  const engine={running,metricsStale:ref(false),refreshMetrics:async()=>{calls++;concurrent++;peak=Math.max(peak,concurrent);await pause(2);concurrent--;}};
  scope.run(()=>useLiveMetrics(engine,active,document,10));
  await pause(25); assert.ok(calls>=1); assert.equal(peak,1);
  active.value='settings'; await nextTick(); await pause(5); const hidden=calls;
  await pause(20); assert.equal(calls,hidden);
  active.value='home'; await nextTick(); await pause(15); assert.ok(calls>hidden);
  document.hidden=true; document.dispatchEvent(new Event('visibilitychange')); await nextTick(); await pause(5); const background=calls;
  await pause(20); assert.equal(calls,background);
  scope.stop(); document.hidden=false; document.dispatchEvent(new Event('visibilitychange')); await pause(20); assert.equal(calls,background);
});
