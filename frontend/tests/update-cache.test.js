import test from 'node:test';
import assert from 'node:assert/strict';
import { effectScope, ref } from 'vue';
import { useModuleUpdate } from '../src/composables/useModuleUpdate.js';
import { readCache, writeCache, NOTES_CACHE_KEY, NOTES_CACHE_TTL, CHECK_CACHE_KEY } from '../src/services/updateCache.js';
import { RELEASES_URL } from '../src/domain/updates.js';

const ok=value=>({code:0,stdout:JSON.stringify(value),stderr:''});
const info=()=>({version:ref('1.2.2'),versionCode:ref(12201),releaseChannel:ref('stable'),verified:ref(true),mismatch:ref(false)});
function fixture(version='1.2.3', preview=false) {
  const name=`AD_Killer_By_NFQUEUE-v${version}.zip`, url=`${RELEASES_URL}/download/v${version}/${name}`;
  return {tag_name:'v'+version,draft:false,prerelease:preview,html_url:`${RELEASES_URL}/tag/v${version}`,body:'GitHub notes <script>alert(1)</script>',
    assets:[{name,size:500,browser_download_url:url},{name:name+'.sha256',browser_download_url:url+'.sha256'}]};
}
function memory() {
  const values=new Map();
  return {values,getItem:key=>values.get(key),setItem:(key,value)=>values.set(key,value),removeItem:key=>values.delete(key)};
}
test('cache schema, TTL, clock rollback, context, oversize/corrupt payload and denied storage are bounded',()=>{
  const storage=memory(), decode=value=>value?.valid ? value : null;
  writeCache(storage,'test',{valid:true},'stable',1000);
  assert.equal(readCache(storage,'test',100,decode,'stable',1100).value.valid,true);
  assert.equal(readCache(storage,'test',100,decode,'stable',1101),null);
  for(const raw of ['bad','null','x'.repeat(70000),JSON.stringify({schema:1,savedAt:1200,context:'stable',response:{valid:true}}),
    JSON.stringify({schema:1,savedAt:1000,context:'preview',response:{valid:true}}),JSON.stringify({schema:1,savedAt:1000,context:'stable',response:{valid:false}})]) {
    storage.setItem('test',raw); assert.equal(readCache(storage,'test',100,decode,'stable',1100),null); assert.equal(storage.getItem('test'),undefined);
  }
  const denied={getItem(){throw Error('denied');},setItem(){throw Error('denied');},removeItem(){throw Error('denied');}};
  assert.equal(readCache(denied,'test',100,decode),null);
  assert.doesNotThrow(()=>writeCache(denied,'test',{valid:true}));
});

test('notes survive app close, avoid network on reopen, manually refresh and retain cached text on failure',async()=>{
  const storage=memory(), commands=[];
  let response=ok([fixture()]);
  const api={command:async command=>{commands.push(command);return response;}};
  let scope=effectScope(), model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  await model.loadReleaseNotes(); assert.ok(storage.getItem(NOTES_CACHE_KEY)); scope.stop();
  scope=effectScope(); model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  try {
    assert.equal(model.latestRelease.value.version,'1.2.3'); assert.equal(model.notesCached.value,true);
    await model.loadReleaseNotes(); assert.equal(commands.length,1);
    response={code:1,stdout:'',stderr:''}; await model.loadReleaseNotes(true);
    assert.equal(model.latestRelease.value.version,'1.2.3'); assert.match(model.notesError.value,/仍显示/);
    response=ok([fixture('1.2.4')]); await model.loadReleaseNotes(true);
    assert.equal(model.latestRelease.value.version,'1.2.4'); assert.equal(model.notesCached.value,false);
  } finally {scope.stop();}
  const record=JSON.parse(storage.getItem(NOTES_CACHE_KEY)); record.savedAt=Date.now()-NOTES_CACHE_TTL-1;
  storage.setItem(NOTES_CACHE_KEY,JSON.stringify(record));
  scope=effectScope(); model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  assert.equal(model.latestRelease.value,null); scope.stop();
});

test('update badge survives app restart, channel/version changes invalidate it, download rechecks cached hints',async()=>{
  const storage=memory(), commands=[];
  let release=fixture(), ready='', checkFails=false;
  const api={command:async command=>{
    commands.push(command);
    if(command==='--jobs-list') return ok({status:'ok',jobs:[]});
    if(command==='--module-update-status') return ok({version:ready,channel:'stable',rebootRequired:false});
    if(command.startsWith('--module-update-check')) return checkFails ? {code:1,stdout:'',stderr:''} : ok(release);
    if(command.startsWith('--module-update-download')) {ready='1.2.3';return ok({status:'started',job_id:'123-456'});}
    return ok({status:'success',exit_code:0});
  },readJob:async()=>''};
  let scope=effectScope(), model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  await model.check(); assert.equal(model.available.value,true); scope.stop();
  commands.length=0;
  scope=effectScope(); model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  try {
    await model.initialize(); assert.equal(model.available.value,true); assert.equal(model.checkCached.value,true);
    assert.deepEqual(commands,['--module-update-status','--jobs-list']);
    await model.download(); assert.deepEqual(commands.slice(2,5),['--module-update-check stable','--jobs-list','--module-update-download 1.2.3']);
    model.setPreview(true); assert.equal(model.available.value,false);
    model.setPreview(false); assert.equal(model.available.value,true);
    checkFails=true; await model.download(); assert.equal(model.available.value,false);
    assert.equal(storage.getItem(CHECK_CACHE_KEY+'-stable'),undefined);
  } finally {scope.stop();}
  checkFails=false; release=fixture(); ready='';
  scope=effectScope(); model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1)); await model.check(); scope.stop();
  const installed=info(); installed.version.value='1.2.3'; installed.versionCode.value=12301;
  scope=effectScope(); model=scope.run(()=>useModuleUpdate(api,installed,{log(){}},storage,1));
  try {await model.initialize(); assert.equal(model.available.value,false); assert.equal(model.release.value,null);} finally {scope.stop();}
});
