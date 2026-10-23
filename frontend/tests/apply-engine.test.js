import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { useEngine, waitEngine } from '../src/composables/useEngine.js';
import { useApps } from '../src/composables/useApps.js';
import { useCustomRules } from '../src/composables/useCustomRules.js';
import { applied } from './fixtures/apply.js';

const old = 'a'.repeat(64), next = 'b'.repeat(64), file = 'c'.repeat(64);
const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
const feedback = messages => ({ log: text => messages?.push(text), toast() {}, consoleOpen: ref(false) });
const baseline = () => ({ running: true, iptables_active: true, runtime: 'running', mode: 2, scope: 0,
  daemon_image_current: true, freezer_state: 'normal', rules_black: 4, rules_white: 3, apply: applied() });

test('failed desired policy shows old effective policy/counts and retries only through apply, never start', async () => {
  const scope = effectScope(), commands = [], messages = [];
  let snapshot = baseline();
  const api = { readFile: async () => 'black=999\nwhite=999', command: async command => {
    commands.push(command);
    if (command === '--apps-apply') return ok({ status: 'queued', revision: next });
    if (command === '--apply-status ' + next) {
      snapshot = { ...snapshot, rules_black: 2, rules_white: 1, apply: applied({ desired_revision: next, applied_revision: next, mode: 1, scope: 1 }) };
      return ok(snapshot.apply);
    }
    return ok(snapshot);
  } };
  const model = scope.run(() => useEngine(api, feedback(messages)));
  try {
    await model.refresh(); assert.equal(model.running.value, true); assert.equal(model.total.value, 7);
    snapshot = { ...snapshot, mode: 1, scope: 1, apply: applied({ status: 'failed', desired_revision: next, error: 'IPv6 switch failed' }) };
    await model.refresh(); assert.equal(model.running.value, false); assert.equal(model.applyIssue.value, true);
    assert.equal(model.kind.value, 'warn'); assert.equal(model.badge.value, '应用失败');
    assert.match(model.description.value, /此前策略仍在运行/); assert.equal(model.total.value, 7);
    assert.equal(model.state.value.scope, 1); assert.equal(model.state.value.apply.scope, 0);
    const before = commands.slice(); await model.control('start'); assert.deepEqual(commands, before);
    const retry = model.retryApply(); assert.equal(model.setting.value, 'apply'); await retry;
    assert.equal(model.running.value, true); assert.equal(model.total.value, 3); assert.equal(model.setting.value, '');
    assert.ok(messages.some(message => message.includes('回执已确认')));
    assert.ok(!commands.some(command => /^--(?:start|stop|daemon)$/.test(command)));
  } finally { scope.stop(); }
});

test('fast ACK changes can revoke health but cannot promote without full route verification', async () => {
  const scope = effectScope(); let snapshot = baseline(), fast = null, failed = false;
  const api = { readFile: async () => 'black=999', command: async command =>
    command === '--status' && failed ? { code: 1, stdout: '', stderr: 'denied' } : ok(command === '--status-fast' ? fast || snapshot : snapshot) };
  const model = scope.run(() => useEngine(api, feedback()));
  try {
    await model.refresh(); assert.equal(model.running.value, true);
    fast = { ...snapshot, apply: applied({ status: 'pending', desired_revision: next }) }; failed = true;
    await model.refreshMetrics(); assert.equal(model.running.value, false); assert.equal(model.applyIssue.value, true);
    assert.equal(model.metricsStale.value, true);
    fast = { ...snapshot, apply: applied({ desired_revision: next, applied_revision: next }) };
    await model.refreshMetrics(); assert.equal(model.running.value, false, 'an approximate check cannot turn a new revision green');
    failed = false; snapshot = fast;
    await model.refreshMetrics(); assert.equal(model.running.value, true);
    snapshot = { ...snapshot, apply: undefined }; fast = snapshot;
    await model.refreshMetrics(); assert.equal(model.running.value, false); assert.equal(model.total.value, null);
    assert.equal(model.canRetryApply.value, true);
    snapshot = { ...snapshot, apply: applied({ applied_revision: next }) };
    await model.refresh(); assert.equal(model.running.value, false, 'inconsistent revision fields are not a receipt');
    snapshot = { ...baseline(), runtime: 'error', last_error: 'owner is fault-closing' };
    await model.refresh(); assert.equal(model.running.value, false); assert.equal(model.kind.value, 'bad');
    assert.equal(model.title.value, '守护正在故障退出');
  } finally { scope.stop(); }
});

test('startup waits for the submitted policy ACK, not just runtime=running or a route', async () => {
  let checks = 0;
  const api = { command: async () => ok({ runtime: 'running', running: true }) };
  const outcome = await waitEngine('running', api, async () => ({ ...baseline(),
    apply: ++checks < 2 ? applied({ status: 'applying', desired_revision: next }) : applied({ desired_revision: next, applied_revision: next }) }),
  { active: true, wait: async () => {} });
  assert.equal(outcome.ok, true); assert.equal(checks, 2);
  assert.equal((await waitEngine('running', api, async () => ({ ...baseline(), apply: undefined }), { active: true, wait: async () => {} })).ok, false);
});

test('editors bind the saved file revision, then observe the distinct submission revision', async () => {
  for (const variant of ['custom', 'allow', 'intercept']) {
    const scope = effectScope(), commands = [], messages = [];
    const api = { readFile: async () => variant === 'custom' ? 'ads.example.com\n' : 'com.example.app\n', packageManager: () => null,
      writeFile: async () => ok({ status: 'ok', revision: file }), command: async command => {
        commands.push(command);
        return command.startsWith('--apply-status') ? ok(applied({ desired_revision: next, applied_revision: next })) : ok({ status: 'queued', revision: next });
      } };
    const model = scope.run(() => variant === 'custom' ? useCustomRules(api, feedback(messages)) : useApps(api, feedback(messages), ref(variant === 'intercept' ? 1 : 0)));
    try {
      await model.load(); await model.save();
      assert.deepEqual(commands, [variant === 'custom' ? '--reload custom ' + file : '--apps-apply ' + variant + ' ' + file, '--apply-status ' + next]);
      assert.ok(messages.some(message => message.includes('回执已确认')));
      assert.equal(model.saving.value, false);
    } finally { scope.stop(); }
  }
});

test('closing during a save preserves the draft and does not dispatch hidden follow-up work or feedback', async () => {
  for (const variant of ['custom', 'apps']) {
    const scope = effectScope(), commands = [], messages = [];
    let finish, started;
    const writing = new Promise(resolve => { started = resolve; });
    const api = { readFile: async () => variant === 'custom' ? 'keep.example.com\n' : 'com.example.app\n', packageManager: () => null,
      writeFile: async () => { started(); return new Promise(resolve => { finish = resolve; }); },
      command: async command => { commands.push(command); return ok({}); } };
    const model = scope.run(() => variant === 'custom' ? useCustomRules(api, feedback(messages)) : useApps(api, feedback(messages), ref(0)));
    await model.load(); const saving = model.save(); await writing;
    scope.stop(); finish(ok({ status: 'ok', revision: file })); await saving;
    assert.equal(model.saving.value, false); assert.equal(commands.length, 0); assert.equal(messages.length, 0);
  }
});
