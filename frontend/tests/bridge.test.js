import assert from 'node:assert/strict';
import test from 'node:test';
import { createBridge, decodeText, encodeText, shellQuote } from '../src/services/bridge.js';
import { createApi, PATHS } from '../src/services/api.js';

test('native three-argument callbacks, concurrency, errors, cleanup and APatch fallback', async () => {
  const host = {};
  const callbacks = [];
  host.ksu = { exec(command, options, callback) {
    assert.equal(arguments.length, 3); assert.deepEqual(JSON.parse(options), {});
    callbacks.push(callback);
    setImmediate(() => host[callback](command === 'fail' ? 7 : 0, command, command === 'fail' ? 'permission denied' : ''));
  } };
  const bridge = createBridge(host);
  const results = await Promise.all(['first', 'second', 'fail'].map(bridge.run));
  assert.deepEqual(results.map(r => r.code), [0, 0, 7]);
  assert.equal(results[2].stderr, 'permission denied');
  assert.equal(new Set(callbacks).size, 3);
  assert.ok(callbacks.every(key => !(key in host)));
  host.ksu = { exec(_, __, callback) { host[callback](undefined, '', 'malformed'); } };
  assert.equal((await bridge.run('invalid-result')).code, -1);
  host.ksu = { exec(command, _, callback) { host[callback](0, command, ''); } };
  host.apatch = host.ksu; delete host.ksu;
  assert.equal((await bridge.run('apatch')).stdout, 'apatch');
  host.apatch = { exec() { throw new Error('disconnected'); } };
  assert.equal((await bridge.run('error')).code, -1);
  assert.equal(Object.keys(host).filter(k => k.startsWith('adKillerNfqueueExec_')).length, 0);
  delete host.apatch;
  assert.match((await bridge.run('missing')).stderr, /KernelSU/);
});

test('callback timeout is unknown result, not success, with cleanup', async () => {
  const host = { ksu: { exec() {} } };
  const result = await createBridge(host, 1).run('unanswered');
  assert.equal(result.code, -1); assert.match(result.stderr, /结果未知/);
  assert.equal(Object.keys(host).length, 1);
});

test('Unicode file I/O and source pipe safely quote shell arguments', async () => {
  assert.equal(decodeText(encodeText('白名单 😀\n@@safe.example.org')), '白名单 😀\n@@safe.example.org');
  assert.equal(shellQuote("a'b"), "'a'\\''b'");
  const commands = [], host = {};
  host.ksu = { exec(command, _, callback) { commands.push(command); host[callback](0, command.includes('base64 ') ? encodeText('内容') : '', ''); } };
  const api = createApi(host);
  assert.equal(await api.readFile(PATHS.custom), '内容');
  await api.writeFile(PATHS.allow, "com.example.app\n");
  await api.saveSources("https://example.org/a'b\n");
  assert.ok(commands[0].includes("'/data/adb/AD_Killer_By_NFQUEUE/custom_rules.txt'"));
  assert.ok(commands[1].includes(" > '/data/adb/AD_Killer_By_NFQUEUE/app_whitelist.txt'"));
  assert.match(commands[2], /base64 -d \| .* --sources-save$/);
});
