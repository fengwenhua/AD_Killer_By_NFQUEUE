import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { useEngine, waitEngine } from '../src/composables/useEngine.js';

const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
test('unknown freezer health stays observable but never green or automatically restarted', async () => {
  const scope = effectScope(), commands = [];
  let state = { running: true, iptables_active: true, runtime: 'running', freezer_state: 'normal', frozen: false, scope: 0 };
  let fullFails = false;
  const api = { readFile: async () => '', command: async command => {
    commands.push(command);
    return command === '--status' && fullFails ? { code: 1, stdout: '', stderr: 'denied' } : ok(state);
  } };
  const model = scope.run(() => useEngine(api, { log() {}, toast() {}, consoleOpen: ref(false) }));
  try {
    await model.refresh(); assert.equal(model.running.value, true);
    state = { ...state, freezer_state: 'unknown', frozen: null, freezer_error: 'cannot read events' };
    await model.refreshMetrics(); assert.equal(model.running.value, false);
    assert.equal(model.healthUnknown.value, true); assert.equal(model.kind.value, 'warn');
    assert.equal(model.state.value.running, true, 'raw process presence must keep read-only polling alive');
    assert.equal(model.badge.value, '状态未核实'); assert.match(model.description.value, /不会自动停止或解冻/);
    await model.control('start');
    assert.ok(commands.every(command => command.startsWith('--status')), 'no recovery mutations');
    const pending = model.verifyHealth(); assert.equal(model.checking.value, true);
    await pending; assert.equal(model.checking.value, false);
    const result = await waitEngine('running', api, model.refresh, { active: true, wait: async () => {} });
    assert.equal(result.ok, false); assert.equal(result.kind, 'query');
    // Fast normal alone cannot promote an unknown observation to healthy.
    fullFails = true; state = { ...state, freezer_state: 'normal', frozen: false };
    await model.refreshMetrics(); assert.equal(model.healthUnknown.value, true);
    assert.equal(model.running.value, false); assert.equal(model.metricsStale.value, true);
    fullFails = false; await model.refreshMetrics();
    assert.equal(model.healthUnknown.value, false); assert.equal(model.running.value, true);
    state = { ...state, running: false, freezer_state: 'frozen', frozen: true, runtime: 'error' };
    await model.refreshMetrics(); assert.equal(model.kind.value, 'bad'); assert.equal(model.running.value, false);
  } finally { scope.stop(); }
});

test('a normal fast observation followed by unknown full verification is not startup success', async () => {
  const result = await waitEngine('running', { command: async () => ok({ runtime: 'running', running: true, freezer_state: 'normal' }) },
    async () => ({ running: true, iptables_active: true, freezer_state: 'unknown' }), { active: true, wait: async () => {} });
  assert.equal(result.ok, false);
});

test('cleanup failure never shows healthy or restarts; explicit stop retry clears the fault', async () => {
  const scope = effectScope(), commands = [];
  let state = { running: true, iptables_active: true, runtime: 'error', last_error_code: 'cleanup_unknown', enabled: false };
  const api = { readFile: async () => '', command: async command => {
    commands.push(command);
    if (command === '--stop') { state = { ...state, running: false, iptables_active: false, runtime: 'stopped', last_error_code: '' }; return ok({ status: 'stopped' }); }
    return ok(state);
  } };
  const model = scope.run(() => useEngine(api, { log() {}, toast() {}, consoleOpen: ref(false) }));
  try {
    state = { ...state, last_error_code: '', runtime: 'running' };
    await model.refresh(); assert.equal(model.running.value, true);
    state = { ...state, last_error_code: 'cleanup_unknown', runtime: 'error' };
    await model.refreshMetrics(); assert.equal(model.running.value, false); assert.equal(model.cleanupFailed.value, true);
    assert.equal(model.title.value, '停止结果待核实'); assert.equal(model.kind.value, 'bad');
    const beforeStart = commands.slice();
    await model.control('start'); assert.deepEqual(commands, beforeStart);
    await model.control('stop'); assert.equal(model.cleanupFailed.value, false); assert.equal(model.kind.value, 'neutral');
  } finally { scope.stop(); }
});
