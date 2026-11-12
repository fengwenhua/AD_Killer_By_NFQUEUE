import test from 'node:test';
import assert from 'node:assert/strict';
import { effectScope,ref } from 'vue';
import { useModuleUpdate } from '../src/composables/useModuleUpdate.js';
import { RELEASES_URL } from '../src/domain/updates.js';
const ok=value=>({code:0,stdout:JSON.stringify(value),stderr:''});
const info=()=>({verified:ref(true),mismatch:ref(false),version:ref('1.2.13'),versionCode:ref(13301),releaseChannel:ref('stable')});
function release(version='1.2.14') {
 const name=`AD_Killer_By_NFQUEUE-v${version}.zip`,url=`${RELEASES_URL}/download/v${version}/${name}`;
 return {tag_name:'v'+version,draft:false,prerelease:false,html_url:`${RELEASES_URL}/tag/v${version}`,assets:[{name,size:500,browser_download_url:url},{name:name+'.sha256',size:96,browser_download_url:url+'.sha256'}]};
}
function fixture({failed=false,unknown=false,other=false}={}) {
 const commands=[];let ready='',reboot=false,stage='download';
 const api={command:async c=>{
  commands.push(c);
  if(c.startsWith('--module-update-check'))return ok(release());
  if(c==='--jobs-list')return ok({status:'ok',jobs:[]});
  if(c==='--module-update-status')return ok({version:ready,channel:'stable',rebootRequired:reboot});
  if(c.startsWith('--module-update-download')){stage='download';ready=other?'1.2.15':'1.2.14';return ok({status:'started',job_id:'123-456'});}
  if(c.startsWith('--module-update-install')){stage='install';reboot=true;return ok({status:'started',job_id:'124-456'});}
  if(c.startsWith('--update-status'))return unknown?{code:1,stdout:''}:ok({status:failed?'failed':'success',exit_code:failed?1:0});
  throw Error('unexpected '+c);
 },readJob:async()=>stage};
 return {commands,api};
}
test('checking and opening confirmation are read-only; one confirmation downloads, verifies and installs the pinned version', async()=>{
 const f=fixture(),scope=effectScope(),m=scope.run(()=>useModuleUpdate(f.api,info(),{log(){}},null,1));
 try {
  await m.check();assert.equal(m.phase.value,'available');m.requestUpgrade();
  assert.equal(m.confirming.value,true);assert.deepEqual(f.commands,['--module-update-check stable']);
  const updating=m.upgrade();assert.equal(m.busy.value,true);await updating;
  assert.equal(m.phase.value,'reboot');assert.equal(m.busy.value,false);
  assert.deepEqual(f.commands.filter(c=>/--module-update-(download|install) /.test(c)),['--module-update-download 1.2.14','--module-update-install 1.2.14']);
 } finally{scope.stop();}
});
test('cancelling an update never downloads or installs',async()=>{
 const f=fixture(),scope=effectScope(),m=scope.run(()=>useModuleUpdate(f.api,info(),{log(){}},null,1));
 try{await m.check();m.requestUpgrade();m.cancelUpgrade();await m.upgrade();assert.equal(m.confirming.value,false);assert.deepEqual(f.commands,['--module-update-check stable']);}finally{scope.stop();}
});
for(const mode of ['failed','unknown','other'])test(`download ${mode} never automatically dispatches installation`,async()=>{
 const f=fixture({[mode]:true}),scope=effectScope(),m=scope.run(()=>useModuleUpdate(f.api,info(),{log(){}},null,0));
 try{await m.check();m.requestUpgrade();await m.upgrade();assert.equal(f.commands.some(c=>c.startsWith('--module-update-install ')),false);assert.equal(m.reboot.value,false);}finally{scope.stop();}
});
test('a previously downloaded older package is hidden after discovering a newer target',async()=>{
 const f=fixture(),scope=effectScope(),m=scope.run(()=>useModuleUpdate(f.api,info(),{log(){}},null,1));
 try{m.ready.value='1.2.14';m.readyChannel.value='stable';assert.equal(m.canInstall.value,true);f.api.command=async c=>c.startsWith('--module-update-check')?ok(release('1.2.15')):ok({});await m.check();assert.equal(m.canInstall.value,false);assert.equal(m.target.value,'1.2.15');assert.equal(m.phase.value,'available');}finally{scope.stop();}
});
test('revalidation that changes the target requires fresh confirmation and dispatches no update',async()=>{
 const f=fixture(),scope=effectScope(),m=scope.run(()=>useModuleUpdate(f.api,info(),{log(){}},null,1));
 try{await m.check();m.checkCached.value=true;m.requestUpgrade();f.api.command=async c=>{f.commands.push(c);return ok(release('1.2.15'));};await m.upgrade();assert.equal(m.target.value,'1.2.15');assert.match(m.message.value,/重新|确认/);assert.equal(f.commands.some(c=>/--module-update-(download|install) /.test(c)),false);}finally{scope.stop();}
});
