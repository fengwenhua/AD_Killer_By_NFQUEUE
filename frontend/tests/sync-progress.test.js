import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { syncProgress } from '../src/domain/syncProgress.js';
import { sourceHistory } from '../src/domain/rules.js';
import { useSync } from '../src/composables/useSync.js';
const id = '123-456';
export function sourceReport(rows, phase = 'processing', job = id) {
  return `job_id=${job}\npublish_status=${phase}\ngeneration=${phase === 'published' ? '777-888' : ''}\nsource_count=${rows.length}\naccepted_sources=${rows.filter(s=>s.status==='accepted').length}\n` + rows.map((row,i)=>`\n[source_${i}]\nurl=https://example.org/${i}\n`+Object.entries(row).map(([k,v])=>`${k}=${v}\n`).join('')).join('');
}
const ok = value => ({code:0,stdout:JSON.stringify(value),stderr:''});
const storage = () => { const m=new Map();return {getItem:k=>m.get(k),setItem:(k,v)=>m.set(k,v),removeItem:k=>m.delete(k)}; };

test('progress separates usable, fresh cache, failures and queued work without promising publication', () => {
  const text=sourceReport([{status:'accepted',provenance:'cache_fresh'},{status:'download_error',reason:'HTTP 404',attempts:3,max_attempts:3},{status:'downloading',attempts:2,max_attempts:3},{status:'queued'}]);
  const p=syncProgress(text,id);
  assert.deepEqual([p.total,p.done,p.ready,p.cached,p.failed,p.pending],[4,2,1,1,1,2]);
  assert.equal(p.rows[0].display.label,'缓存已就绪');assert.doesNotMatch(p.rows[0].display.label,/失败|未确认/);
  assert.equal(p.rows[1].display.label,'下载失败');assert.match(p.rows[1].display.detail,/地址不存在/);
  assert.match(p.rows[2].display.detail,/第 2 次/);
  assert.doesNotMatch(p.title,/完成|保存/);
});
test('job-bound bounded progress rejects stale, duplicate, malformed or hostile sources', () => {
  const valid=sourceReport([{status:'accepted'}]);
  for(const text of [sourceReport([{status:'accepted'}],'processing','999-000'),valid.replace('source_count=1','source_count=2'),valid.replace('status=accepted','status=unknown'),valid.replace('https://example.org/0','javascript:alert(1)'), 'x'.repeat(512*1024+1)])
    assert.equal(syncProgress(text,id),null);
  assert.equal(syncProgress(sourceReport([{status:'accepted'},{status:'accepted',url:'https://example.org/0'}]),id),null);
});
test('interrupted snapshots retain completed work and turn pending work into unfinished, never success', () => {
  const p=syncProgress(sourceReport([{status:'accepted'},{status:'downloading'},{status:'queued'}]),id,'interrupted');
  assert.equal(p.done,1);assert.match(p.title,/中断/);assert.equal(p.rows[1].display.label,'未完成');assert.equal(p.rows[2].display.pending,false);
  const failed=syncProgress(sourceReport([{status:'accepted'}],'published'),id,'failed');
  assert.doesNotMatch(failed.title,/同步完成/);assert.equal(failed.kind,'warning');
});
test('fresh reuse is healthy while failed-download fallback remains actionable, even before publication', () => {
  for(const phase of ['incomplete_sources','published']) {
    const p=syncProgress(sourceReport([{status:'accepted',provenance:'cache_fresh'},{status:'accepted',provenance:'cache_fallback',warning:'HTTP 503',cache_age_seconds:600,cache_verified_at:1700000000}],phase),id,phase==='published'?'success':'failed');
    assert.equal(p.rows[0].display.needsAction,false);assert.equal(p.rows[1].display.needsAction,true);assert.equal(p.needsRetry,1);
    assert.equal(p.rows[1].display.kind,'warning');
  }
  assert.equal(sourceHistory({enabled:false,url:'https://example.org/1'},[]).needsAction,false);
});
test('live sync exposes source results independently of logs and restores terminal history without a new completion toast', async () => {
  const running=sourceReport([{status:'accepted',provenance:'cache_fresh'},{status:'downloading'}]);
  const finished=sourceReport([{status:'accepted',provenance:'cache_fresh'},{status:'download_error',reason:'HTTP 404',attempts:3,max_attempts:3}],'incomplete_sources');
  const scope=effectScope(),history=ref([]),toasts=[],commands=[];let calls=0;
  const api={command:async c=>{commands.push(c);return c==='--jobs-list'?ok({status:'ok',jobs:[]}):c==='--update-start'?ok({status:'started',job_id:id}):ok({status:++calls===1?'running':'failed',exit_code:calls===1?null:1,job_id:id,type:'rules',progress:calls===1?running:finished});},readJob:async()=>'',readFile:async()=>finished};
  const model=scope.run(()=>useSync(api,{log(){},toast:v=>toasts.push(v),consoleOpen:ref(false)},history,async()=>{},storage(),1));
  try {
    await model.start();assert.equal(model.progress.value.failed,1);assert.equal(history.value[0].status,'accepted');
    assert.match(model.message.value,/规则未更新/);assert.match(model.message.value,/原规则/);assert.equal(model.busy.value,false);
    assert.equal(commands.filter(c=>c==='--update-start').length,1);assert.equal(toasts.length,1);
  } finally {scope.stop();}
});
