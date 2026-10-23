import assert from 'node:assert/strict';
import test from 'node:test';
import { observeApply, validApplyRevision } from '../src/domain/apply.js';

const revision = 'a'.repeat(64), old = 'b'.repeat(64);
const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
const queued = ok({ status: 'queued', revision });
const state = (status, extra = {}) => ok({ status, verification: 'owner_and_commit',
  desired_revision: revision, applied_revision: old, verified: true, ...extra });
const life = () => ({ active: true, wait: async () => {} });

test('submission is not success: only the exact owner-bound applied revision is accepted', async () => {
  const calls = [], replies = [state('pending'), state('applying'), state('applied', { applied_revision: revision })];
  const result = await observeApply({ command: async command => { calls.push(command); return replies.shift(); } }, queued, life());
  assert.equal(result.phase, 'applied'); assert.equal(calls.length, 3);
  assert.ok(calls.every(command => command === '--apply-status ' + revision));
  for (const response of [state('applied'), state('applied', { applied_revision: revision, verified: false }),
    state('applied', { applied_revision: revision, verification: 'entry-only' })]) {
    assert.equal((await observeApply({ command: async () => response }, queued, life(), { attempts: 3 })).phase, 'unknown');
  }
  assert.equal(validApplyRevision('a; reboot'), false);
});

test('failed, deferred and superseded requests never announce applied or restart the engine', async () => {
  const replies = [state('failed', { error: '<script>bad IPv6</script>' }), state('deferred'), state('superseded')];
  for (const response of replies) {
    const commands = [];
    const outcome = await observeApply({ command: async command => { commands.push(command); return response; } }, queued, life());
    assert.equal(outcome.phase, JSON.parse(response.stdout).status);
    assert.ok(commands.every(command => command.startsWith('--apply-status ')));
    if (outcome.phase === 'failed') { assert.match(outcome.message, /此前的策略/); assert.match(outcome.message, /bad IPv6/); }
  }
  assert.equal((await observeApply({}, ok({ status: 'deferred', revision }), life())).phase, 'deferred');
});

test('query errors, stale responses and a long pending apply stop observation with an honest result', async () => {
  let calls = 0;
  const failed = await observeApply({ command: async () => { calls++; throw new Error('denied'); } }, queued, life());
  assert.equal(failed.phase, 'unknown'); assert.equal(calls, 3);
  calls = 0;
  const pending = await observeApply({ command: async () => { calls++; return state('applying'); } }, queued, life(), { attempts: 2 });
  assert.equal(pending.phase, 'pending'); assert.equal(calls, 2);
  assert.equal((await observeApply({ command: async () => state('applied', { desired_revision: old }) }, queued, life(), { attempts: 3 })).phase, 'unknown');
  assert.equal((await observeApply({}, ok({ status: 'reloaded' }), life())).phase, 'unknown');
});

test('closing during paint or during a bridge query prevents all late success feedback', async () => {
  const closed = life(); let calls = 0;
  closed.wait = async () => { closed.active = false; };
  assert.equal((await observeApply({ command: async () => { calls++; return state('applied'); } }, queued, closed)).phase, 'cancelled');
  assert.equal(calls, 0);
  const querying = life();
  assert.equal((await observeApply({ command: async () => { querying.active = false; return state('applied', { applied_revision: revision }); } }, queued, querying)).phase, 'cancelled');
  let time = 0;
  assert.equal((await observeApply({ command: async () => state('pending') }, queued, life(), { now: () => time += 100, timeoutMs: 150 })).phase, 'pending');
});
