import test from 'node:test';
import assert from 'node:assert/strict';
import { effectScope, ref } from 'vue';
import { compareVersions, parseRelease, selectRelease, RELEASES_URL } from '../src/domain/updates.js';
import { useModuleUpdate } from '../src/composables/useModuleUpdate.js';

function fixture(version = '1.2', prerelease = false) {
  const name = `AD_Killer_By_NFQUEUE-v${version}.zip`;
  const url = `${RELEASES_URL}/download/v${version}/${name}`;
  return { tag_name: 'v'+version, draft:false, prerelease, html_url:`${RELEASES_URL}/tag/v${version}`,
    body:'Release notes <img src=x onerror=alert(1)>',assets:[{name,size:500,browser_download_url:url},{name:name+'.sha256',size:96,browser_download_url:url+'.sha256'}] };
}
const ok = value => ({code:0,stdout:JSON.stringify(value),stderr:''});
const info = () => ({verified:ref(true),mismatch:ref(false),version:ref('1.1')});

test('release policy validates exact repository assets, bounds, draft/preview and numeric versions', () => {
  assert.equal(compareVersions('1.10','1.9'),1);
  assert.equal(compareVersions('1.0','1.0.0'),0);
  assert.throws(() => compareVersions('1.0;id','1.0'));
  assert.equal(parseRelease(fixture()).version,'1.2');
  for (const bad of [null,{...fixture(),draft:true},{...fixture(),assets:[]},fixture('../x'),{...fixture(),html_url:'https://evil.example'}]) assert.equal(parseRelease(bad),null);
  const malicious = fixture(); malicious.assets[0].browser_download_url = 'https://evil.example/x.zip';
  assert.equal(parseRelease(malicious),null);
  assert.equal(selectRelease([fixture('1.2'),fixture('1.3',true)],false).version,'1.2');
  assert.equal(selectRelease([fixture('1.2'),fixture('1.3',true)],true).version,'1.3');
  const metadata={module:'AD_Killer_By_NFQUEUE',version:'v1.2',versionCode:120,sha256:'a'.repeat(64),
    zipUrl:`${RELEASES_URL}/download/v1.2/AD_Killer_By_NFQUEUE-v1.2.zip`,changelog:`${RELEASES_URL}/download/v1.2/changelog.md`};
  assert.equal(selectRelease({update:metadata},false).version,'1.2');
  assert.equal(selectRelease({update:{...metadata,sha256:'bad'}},false),null);
  assert.equal(selectRelease({update:{...metadata,zipUrl:'https://evil.example/x'}},false),null);
});

test('manual check, verified download, explicit install, status and background resume', async () => {
  const scope = effectScope(), commands = [], values = new Map();
  const storage = { getItem:k=>values.get(k), setItem:(k,v)=>values.set(k,v), removeItem:k=>values.delete(k) };
  let ready = '', reboot = false, install = false;
  const api = { command: async command => {
    commands.push(command);
    if (command.startsWith('--module-update-check')) return ok(fixture());
    if (command === '--module-update-status') return ok({version:ready,rebootRequired:reboot});
    if (command.startsWith('--module-update-download')) { ready='1.2'; return ok({status:'started',job_id:'123-456'}); }
    if (command.startsWith('--module-update-install')) { install=true; reboot=true; return ok({status:'started',job_id:'124-456'}); }
    if (command.startsWith('--update-status')) return ok({status:'success',exit_code:0});
    throw Error(command);
  },readJob:async()=> 'verified'};
  const model = scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  try {
    assert.equal(commands.length,0,'no network check on mount');
    await model.resume(); assert.equal(model.ready.value,'');
    await model.check(); assert.equal(model.available.value,true);
    await model.download(); assert.equal(model.canInstall.value,true); assert.equal(install,false);
    await model.install(); assert.equal(model.reboot.value,true); assert.equal(model.canInstall.value,false);
    assert.equal(values.size,0);
    values.set('ad-killer-by-nfqueue-module-update',JSON.stringify({id:'124-456',version:'1.2',install:true}));
    await model.resume(); assert.equal(values.size,0); assert.match(model.message.value,/重启/);
  } finally { scope.stop(); }
});

test('unverified/mismatched builds cannot download, failed check clears stale release, downgrade blocked', async () => {
  const scope=effectScope(), module=info(), commands=[];
  let result=ok(fixture());
  const model=scope.run(()=>useModuleUpdate({command:async cmd=>{commands.push(cmd);return result;}},module,{log(){}},null,1));
  try {
    await model.check(); assert.equal(model.available.value,true);
    module.verified.value=false; await model.download(); assert.equal(commands.length,1);
    module.verified.value=true; module.mismatch.value=true; await model.download(); assert.equal(commands.length,1);
    module.mismatch.value=false;
    result={code:1,stdout:'',stderr:'rate limited'}; await model.check(); assert.equal(model.release.value,null); assert.equal(model.kind.value,'error');
    result=ok(fixture('1.0')); await model.check(); assert.equal(model.available.value,false);
  } finally { scope.stop(); }
});

test('late responses are discarded after disposal and failed jobs never enable installation', async () => {
  const scope=effectScope(); let resolve;
  const model=scope.run(()=>useModuleUpdate({command:()=>new Promise(done=>{resolve=done;})},info(),{log(){}},null,1));
  const pending=model.check(); scope.stop(); resolve(ok(fixture())); await pending;
  assert.equal(model.release.value,null);
  const other=effectScope();
  const failed=other.run(()=>useModuleUpdate({command:async cmd=>cmd.startsWith('--module-update-check')?ok(fixture()):cmd.startsWith('--module-update-download')?ok({status:'started',job_id:'1-2'}):ok({status:'failed',exit_code:1}),readJob:async()=> 'SHA mismatch'},info(),{log(){}},null,1));
  try { await failed.check(); await failed.download(); assert.equal(failed.canInstall.value,false); assert.equal(failed.kind.value,'error'); }
  finally { other.stop(); }
});

test('missing persisted job is cleared and recovered package remains explicitly installable', async () => {
  const scope=effectScope(), values=new Map([['ad-killer-by-nfqueue-module-update',JSON.stringify({id:'1-2',version:'1.2',install:false})]]);
  const storage={getItem:k=>values.get(k),removeItem:k=>values.delete(k)};
  const api={command:async cmd=>cmd==='--module-update-status'?ok({version:'1.2',rebootRequired:false}):{code:1,stdout:'{"status":"not_found"}',stderr:''}};
  const model=scope.run(()=>useModuleUpdate(api,info(),{log(){}},storage,1));
  try { await model.resume(); assert.equal(values.size,0); assert.equal(model.busy.value,false); assert.match(model.message.value,/记录已不存在/); }
  finally { scope.stop(); }
});
