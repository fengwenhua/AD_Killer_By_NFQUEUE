import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope } from 'vue';
import { concurrencyNumber, useSyncSettings } from '../src/composables/useSyncSettings.js';
const reply = concurrency => ({ code: 0, stdout: JSON.stringify({ status: 'ok', concurrency }) });

test('concurrency accepts only whole numbers 1–20', () => {
  for (const value of ['1', '10', '20']) assert.equal(concurrencyNumber(value), Number(value));
  for (const value of ['', '0', '21', '-1', '1.5', '2e1', ' 10', null]) assert.equal(concurrencyNumber(value), null);
});
test('saved concurrency comes from the module on every reopen and never starts a sync or daemon', async () => {
  let saved = 10; const commands = [];
  const api = { async command(cmd) { commands.push(cmd); if (cmd.startsWith('--sync-concurrency ')) saved = Number(cmd.split(' ')[1]); return reply(saved); } };
  for (const value of [20, 1]) {
    const scope = effectScope(), model = scope.run(() => useSyncSettings(api));
    try {
      await model.load(); assert.equal(model.count.value, saved);
      model.draft.value = String(value); await model.save();
      assert.equal(model.count.value, value); assert.match(model.message.value, /下次同步/);
      assert.equal(model.busy.value, false);
    } finally { scope.stop(); }
  }
  assert.deepEqual(commands, ['--sync-settings', '--sync-concurrency 20', '--sync-settings', '--sync-concurrency 1']);
});
test('bad input, unsupported reads and failed saves preserve the last confirmed value', async () => {
  const scope = effectScope(), commands = []; let fail = false;
  const model = scope.run(() => useSyncSettings({ async command(cmd) { commands.push(cmd); return fail ? { code: 1, stdout: '{}' } : reply(10); } }));
  try {
    await model.load(); model.draft.value = '21'; await model.save(); assert.equal(commands.length, 1);
    model.draft.value = '5'; fail = true; await model.save();
    assert.equal(model.count.value, 10); assert.ok(model.error.value); assert.equal(model.message.value, '');
    await model.load(); assert.equal(model.ready.value, false); await model.save(); assert.equal(commands.length, 3);
  } finally { scope.stop(); }
});
test('saving paints loading state and closing the UI during paint dispatches nothing', async () => {
  const scope = effectScope(), commands = []; let finishPaint;
  const model = scope.run(() => useSyncSettings({ async command(cmd) { commands.push(cmd); return reply(10); } }, () => new Promise(resolve => { finishPaint = resolve; })));
  await model.load(); model.draft.value = '20'; const saving = model.save();
  assert.equal(model.busy.value, true); assert.deepEqual(commands, ['--sync-settings']);
  scope.stop(); finishPaint(); await saving;
  assert.deepEqual(commands, ['--sync-settings']);
});
