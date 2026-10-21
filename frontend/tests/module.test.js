import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope } from 'vue';
import { parseModuleInfo } from '../src/domain/module.js';
import { useModuleInfo } from '../src/composables/useModuleInfo.js';

const build = { id: 'AD_Killer_By_NFQUEUE', version: '1.0', versionCode: 100, author: 'AD_Killer_By_NFQUEUE contributors' };
const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });

test('version metadata rejects other modules, malformed versions and invalid codes', () => {
  const valid = { module: 'AD_Killer_By_NFQUEUE', version: '1.0', versionCode: 100 };
  assert.deepEqual(parseModuleInfo(valid), { version: '1.0', versionCode: 100, releaseChannel: 'unknown' });
  assert.equal(parseModuleInfo({...valid,releaseChannel:'preview'}).releaseChannel,'preview');
  assert.equal(parseModuleInfo({...valid,releaseChannel:'stable'}).releaseChannel,'stable');
  assert.equal(parseModuleInfo({...valid,releaseChannel:'other'}),null);
  for (const bad of [null, { ...valid, module: 'other' }, { ...valid, version: '<img>' }, { ...valid, version: ['1.0'] }, { ...valid, versionCode: '100' }, { ...valid, versionCode: -1 }]) assert.equal(parseModuleInfo(bad), null);
});

test('installed version and mismatch are honest; fallback remains available offline and never starts engine', async () => {
  const scope = effectScope(), commands = [];
  let response = ok({ module: 'AD_Killer_By_NFQUEUE', version: '0.9', versionCode: 90 });
  const model = scope.run(() => useModuleInfo({ command: async command => { commands.push(command); return response; } }, build));
  try {
    assert.equal(model.version.value, build.version); assert.equal(model.verified.value, false);
    await model.load(); assert.equal(model.version.value, '0.9'); assert.equal(model.mismatch.value, true);
    response = { code: 1, stdout: '', stderr: 'unavailable' }; await model.load();
    assert.equal(model.version.value, build.version); assert.equal(model.verified.value, false);
    response = ok({ module: 'AD_Killer_By_NFQUEUE', version: build.version, versionCode: build.versionCode }); await model.load();
    assert.equal(model.verified.value, true); assert.equal(model.mismatch.value, false);
    assert.deepEqual(commands, Array(3).fill('--version-status'));
  } finally { scope.stop(); }
});

test('late version response cannot publish after disposal, rejected bridge falls back safely', async () => {
  const scope = effectScope(); let resolve;
  const model = scope.run(() => useModuleInfo({ command: () => new Promise(done => { resolve = done; }) }, build));
  const pending = model.load(); scope.stop(); resolve(ok({ module: 'AD_Killer_By_NFQUEUE', version: '9.9.9', versionCode: 999 })); await pending;
  assert.equal(model.verified.value, false); assert.equal(model.loading.value, false);
  const other = effectScope(), fallback = other.run(() => useModuleInfo({ command: async () => { throw new Error('no bridge'); } }, build));
  try { await fallback.load(); assert.equal(fallback.version.value, build.version); }
  finally { other.stop(); }
});
