import assert from 'node:assert/strict';
import test from 'node:test';
import { createHash } from 'node:crypto';
import { effectScope, ref } from 'vue';
import { createApi, PATHS } from '../src/services/api.js';
import { useCustomRules } from '../src/composables/useCustomRules.js';
import { useApps } from '../src/composables/useApps.js';

const revision = text => createHash('sha256').update(text).digest('hex');
const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
const feedback = () => ({ log() {}, toast() {} });
function bridge(run) {
  const commands = [];
  const host = { ksu: { exec(command, _options, callback) {
    commands.push(command);
    const result = run(command);
    host[callback](result.code, result.stdout, result.stderr);
  } } };
  return { api: createApi(host), commands };
}

test('failed native read cannot turn a rule editor ready or overwrite prior rules', async () => {
  // Migrated from tests/audit/probe_frontend.mjs: this now asserts the fix.
  const native = bridge(() => ({ code: 1, stdout: '', stderr: 'permission denied' }));
  let existing = 'keep.example.com\n', writes = 0, reloads = 0;
  const api = { ...native.api,
    writeFile: async (_path, text) => { writes++; existing = text; return ok({}); },
    command: async () => { reloads++; return ok({}); },
  };
  const editor = useCustomRules(api, feedback());
  await editor.load(); assert.equal(editor.ready.value, false);
  editor.black.value = 'draft.example.com'; await editor.save();
  assert.equal(existing, 'keep.example.com\n'); assert.equal(writes, 0); assert.equal(reloads, 0);
});

test('allow and intercept read failures disable saving; a genuine empty file can recover', async () => {
  for (const [scopeValue, target] of [[0, 'allow'], [1, 'intercept']]) {
    let fail = true;
    const native = bridge(command => {
      assert.match(command, new RegExp(`--config-read ${target}$`));
      return fail ? { code: 1, stdout: '', stderr: 'denied' }
        : ok({ status: 'ok', target, content: '', revision: revision('') });
    });
    const scope = effectScope(); let writes = 0;
    const model = scope.run(() => useApps({ ...native.api,
      writeFile: async () => { writes++; return ok({}); },
    }, feedback(), ref(scopeValue)));
    try {
      await model.load(); await model.save();
      assert.equal(model.ready.value, false); assert.equal(model.loading.value, false); assert.equal(writes, 0);
      fail = false; await model.load();
      assert.equal(model.ready.value, true); assert.equal(model.selected.value.size, 0);
    } finally { scope.stop(); }
  }
});

test('missing, malformed, wrong-target and old backend reads never authorize saving', async () => {
  for (const response of [
    { code: 1, stdout: '', stderr: 'unknown command' },
    { code: 0, stdout: 'not JSON', stderr: '' },
    ok({ status: 'missing', target: 'custom', content: '', revision: 'missing' }),
    ok({ status: 'ok', target: 'allow', content: '', revision: 'a'.repeat(64) }),
    ok({ status: 'ok', target: 'custom', content: '', revision: 'invalid' }),
  ]) {
    const native = bridge(() => response);
    await assert.rejects(native.api.readFile(PATHS.custom));
    assert.notEqual((await native.api.writeFile(PATHS.custom, '')).code, 0);
    assert.equal(native.commands.length, 1);
  }
});

test('revisions belong to individual fixed targets; failed rereads revoke permission', async () => {
  let fail = false;
  const native = bridge(command => fail ? { code: 1, stdout: '', stderr: 'denied' }
    : ok({ status: 'ok', target: command.split(' ').at(-1), content: '', revision: revision('') }));
  assert.notEqual((await native.api.writeFile(PATHS.custom, '')).code, 0);
  await native.api.readFile(PATHS.custom);
  assert.notEqual((await native.api.writeFile(PATHS.allow, '')).code, 0);
  assert.notEqual((await native.api.writeFile('/arbitrary/file', '')).code, 0);
  fail = true; await assert.rejects(native.api.readFile(PATHS.custom));
  assert.notEqual((await native.api.writeFile(PATHS.custom, '')).code, 0);
  assert.equal(native.commands.length, 2);
});

test('two windows preserve drafts and stale revisions until explicit reread', async () => {
  let content = 'old.example.com\n';
  const run = command => {
    if (command.endsWith('--config-read custom')) return ok({ status: 'ok', target: 'custom', content, revision: revision(content) });
    const [, encoded, supplied] = command.match(/^printf '%s' '([A-Za-z0-9+/=]*)'.*--config-save custom ([a-f0-9]{64})$/);
    if (supplied !== revision(content)) return { code: 2, stdout: JSON.stringify({ status: 'conflict', message: '配置已被其他窗口修改' }), stderr: '' };
    content = Buffer.from(encoded, 'base64').toString('utf8');
    return ok({ status: 'ok', target: 'custom', revision: revision(content) });
  };
  const first = bridge(run), second = bridge(run);
  const one = useCustomRules(first.api, feedback()), two = useCustomRules(second.api, feedback());
  await one.load(); await two.load();
  // Reload is not the subject of this test; route application is covered separately.
  first.api.command = async () => ok({});
  one.black.value = 'one.example.com'; await one.save();
  two.black.value = 'two.example.com'; await two.save(); await two.save();
  assert.equal(content, 'one.example.com\n'); assert.equal(two.black.value, 'two.example.com');
  const attempts = second.commands.filter(c => c.includes('--config-save'));
  assert.equal(attempts.length, 2); assert.equal(attempts[0], attempts[1]);
  await two.load(); assert.equal(two.black.value, 'one.example.com');
});

test('Unicode text uses stdin protocol and uncertain commits never report success', async () => {
  const previous = revision(''), text = '# 中文 😀\n@@||safe.example.org^\n';
  const native = bridge(command => {
    if (command.includes('--config-read')) return ok({ status: 'ok', target: 'custom', content: '', revision: previous });
    assert.equal(Buffer.from(command.match(/^printf '%s' '([^']*)'/)[1], 'base64').toString('utf8'), text);
    assert.ok(command.endsWith(`--config-save custom ${previous}`)); assert.ok(!command.includes(' > '));
    return { code: 1, stdout: JSON.stringify({ status: 'durability_unknown', target: 'custom', committed: true, revision: revision(text), message: '请重新读取核对' }), stderr: '' };
  });
  await native.api.readFile(PATHS.custom);
  for (let i = 0; i < 2; i++) {
    const result = await native.api.writeFile(PATHS.custom, text);
    assert.notEqual(result.code, 0); assert.match(result.stderr, /重新读取/);
  }
});
