import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope } from 'vue';
import { concurrencyNumber, intervalNumber, retriesNumber, useSyncSettings } from '../src/composables/useSyncSettings.js';
const reply = (concurrency = 10, interval_minutes = 30, retries = 2) => ({ code: 0, stdout: JSON.stringify({ status: 'ok', concurrency, interval_minutes, retries }) });

test('sync settings validate whole numbers and independent ranges', () => {
  for (const value of ['1', '10', '20']) assert.equal(concurrencyNumber(value), Number(value));
  for (const value of ['', '0', '21', '-1', '1.5', '2e1', ' 10', null]) assert.equal(concurrencyNumber(value), null);
  for (const value of ['0', '30', '1440']) assert.equal(intervalNumber(value), Number(value));
  for (const value of ['0', '2', '5']) assert.equal(retriesNumber(value), Number(value));
  for (const value of ['', '-1', '1.5', '1e2', ' 2', null]) { assert.equal(intervalNumber(value), null); assert.equal(retriesNumber(value), null); }
  assert.equal(intervalNumber('1441'), null); assert.equal(retriesNumber('6'), null);
});
test('all three settings are saved together, restored on reopen and never start sync or daemon', async () => {
  let saved = [10, 30, 2]; const commands = [];
  const api = { async command(cmd) { commands.push(cmd); if (cmd.startsWith('--sync-options ')) saved = cmd.split(' ').slice(1).map(Number); return reply(...saved); } };
  for (const values of [[20, 45, 3], [1, 0, 0]]) {
    const scope = effectScope(), model = scope.run(() => useSyncSettings(api));
    try {
      await model.load(); assert.deepEqual([model.count.value, model.interval.value, model.retries.value], saved);
      [model.draft.value, model.intervalDraft.value, model.retriesDraft.value] = values.map(String); await model.save();
      assert.deepEqual([model.count.value, model.interval.value, model.retries.value], values);
      assert.match(model.message.value, /下次同步/); assert.equal(model.busy.value, false);
    } finally { scope.stop(); }
  }
  assert.deepEqual(commands, ['--sync-settings', '--sync-options 20 45 3', '--sync-settings', '--sync-options 1 0 0']);
});
test('invalid fields and failed saves preserve confirmed settings', async () => {
  const scope = effectScope(), commands = []; let fail = false;
  const model = scope.run(() => useSyncSettings({ async command(cmd) { commands.push(cmd); return fail ? { code: 1, stdout: '{}' } : reply(); } }));
  try {
    await model.load();
    for (const [field, value] of [['draft', '21'], ['intervalDraft', '-1'], ['retriesDraft', '6']]) {
      model[field].value = value; await model.save(); assert.equal(commands.length, 1); assert.ok(model.error.value);
      model.draft.value = '10'; model.intervalDraft.value = '30'; model.retriesDraft.value = '2';
    }
    model.draft.value = '5'; fail = true; await model.save();
    assert.deepEqual([model.count.value, model.interval.value, model.retries.value], [10, 30, 2]); assert.ok(model.error.value); assert.equal(model.message.value, '');
    await model.load(); assert.equal(model.ready.value, false); await model.save(); assert.equal(commands.length, 3);
  } finally { scope.stop(); }
});
test('old or mismatched settings replies never imply new options were saved', async () => {
  const scope = effectScope(); let response = { code: 0, stdout: '{"status":"ok","concurrency":10}' };
  const model = scope.run(() => useSyncSettings({ command: async () => response }));
  try {
    await model.load(); assert.equal(model.ready.value, false);
    response = reply(); await model.load(); model.intervalDraft.value = '60'; await model.save();
    assert.equal(model.interval.value, 30); assert.ok(model.error.value); assert.equal(model.message.value, '');
  } finally { scope.stop(); }
});
test('saving paints loading state and disposal during paint dispatches nothing', async () => {
  const scope = effectScope(), commands = []; let finishPaint;
  const model = scope.run(() => useSyncSettings({ async command(cmd) { commands.push(cmd); return reply(); } }, () => new Promise(resolve => { finishPaint = resolve; })));
  await model.load(); model.draft.value = '20'; const saving = model.save();
  assert.equal(model.busy.value, true); assert.deepEqual(commands, ['--sync-settings']);
  scope.stop(); finishPaint(); await saving; assert.deepEqual(commands, ['--sync-settings']);
});
