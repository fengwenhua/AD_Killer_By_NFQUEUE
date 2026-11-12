import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { startupDiagnosis } from '../src/domain/engineDiagnostics.js';
import { useEngine } from '../src/composables/useEngine.js';

const report = () => ({status:'ok',schema:1,read_only:true,authorized:true,running:false,connmark_mask:'0xcc000000',
  last_error_code:'iptables_failed',last_error:'old failure',checks:[{family:'ipv4',status:'clear',chain:''},{family:'ipv6',status:'clear',chain:''}]});
const ok = value => ({code:0,stdout:JSON.stringify(value),stderr:''});

test('startup diagnosis distinguishes clear, conflict and incomplete reads without promising a repaired engine', () => {
  const value = report(); assert.match(startupDiagnosis(value).title,/连接标记检查通过/);
  assert.match(startupDiagnosis(value).advice,/不会自动启动/);
  value.checks[1]={family:'ipv6',status:'conflict',chain:'tc_limiter_OUTPUT'};
  assert.match(startupDiagnosis(value).title,/WLAN/);
  value.checks[1].chain='OTHER_FIREWALL'; assert.equal(startupDiagnosis(value).kind,'bad');
  value.checks[0].status='unknown'; assert.equal(startupDiagnosis(value).kind,'warn');
  assert.match(startupDiagnosis(value).advice,/不能当作没有冲突/);
  value.authorized=false; assert.match(startupDiagnosis(value).title,/授权/);
});
test('malformed or one-family reports are never clear', () => {
  for (const item of [null,{}, {...report(),read_only:false}, {...report(),connmark_mask:''},
    {...report(),checks:[report().checks[0]]}, {...report(),checks:[report().checks[0],report().checks[0]]}]) {
    assert.throws(()=>startupDiagnosis(item));
  }
  for (const code of ['rules_empty','rule_load_failed','queue_conflict','cgroup_isolation_failed','recovery_stop_timeout','cleanup_unknown']) {
    assert.equal(startupDiagnosis({...report(),last_error_code:code}).kind,'warn');
  }
});
test('user-requested self-check paints loading, retains history, and sends only the read-only command', async () => {
  const scope=effectScope(), commands=[]; let complete;
  const api={readFile:async()=>'',command:async c=>{commands.push(c);return c==='--diagnose'?new Promise(resolve=>{complete=resolve;}):ok({running:false,runtime:'error',rules_black:1,rules_white:0});}};
  const model=scope.run(()=>useEngine(api,{log(){},toast(){},consoleOpen:ref(false)}));
  try {
    await model.refresh(); commands.length=0;
    const pending=model.diagnose(); assert.equal(model.diagnosing.value,true);
    await new Promise(resolve=>setTimeout(resolve,50));
    await model.control('start'); assert.deepEqual(commands,['--diagnose']);
    complete(ok(report())); await pending;
    assert.equal(model.diagnosing.value,false); assert.equal(model.diagnosis.value.report.last_error,'old failure');
    assert.equal(model.state.value.running,false,'diagnosis must not turn a stopped engine green');
  } finally { scope.stop(); }
});
test('closing before dispatch cancels the self-check, and old/failed native queries remain visible errors', async () => {
  const commands=[],scope=effectScope();
  const api={readFile:async()=>'',command:async c=>{commands.push(c);return c==='--diagnose'?{code:1,stdout:'',stderr:'unsupported'}:ok({running:false,rules_black:1,rules_white:0});}};
  const model=scope.run(()=>useEngine(api,{log(){},toast(){},consoleOpen:ref(false)}));
  await model.refresh(); const wait=model.diagnose(); scope.stop(); await wait;
  assert.deepEqual(commands,['--status']);
  const second=effectScope(),live=second.run(()=>useEngine(api,{log(){},toast(){},consoleOpen:ref(false)}));
  try { await live.refresh(); await live.diagnose(); assert.equal(live.diagnosis.value,null); assert.match(live.diagnosisError.value,/unsupported/); }
  finally { second.stop(); }
});
