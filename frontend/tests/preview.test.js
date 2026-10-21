import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { useSourcePreview } from '../src/composables/useSourcePreview.js';
import { useUiPreferences } from '../src/composables/useUiPreferences.js';
import { shellQuote } from '../src/services/bridge.js';
import { HAGEZI_ID } from '../src/domain/rules.js';

const ok = previews => ({ code: 0, stdout: JSON.stringify({ status: 'ok', previews }), stderr: '' });
const source = { url: "https://example.org/a'b" };

test('preview opens before bridge work, defaults to 500 Unicode points, caches and safely quotes', async () => {
  const scope = effectScope(), commands = [];
  let painted;
  const model = scope.run(() => useSourcePreview({ command: async command => {
    commands.push(command); return ok([{ url: source.url, text: '中😀'.repeat(501), truncated: true }]);
  } }, ref(500), () => new Promise(resolve => { painted = resolve; })));
  try {
    const loading = model.open(source);
    assert.equal(model.active.value.url, source.url); assert.equal(model.pending.value, true);
    assert.equal(commands.length, 0, 'must allow dialog paint before native exec');
    painted(); await loading;
    const entry = model.active.value.items[0];
    assert.equal(Array.from(entry.text).length, 500); assert.ok(entry.at);
    assert.equal(model.pending.value, false);
    model.close(); assert.equal(model.active.value, null);
    const cached = model.open(source); painted(); await cached;
    assert.equal(commands.length, 1);
    const refreshed = model.refresh(); painted(); await refreshed;
    assert.deepEqual(commands, Array(2).fill('--source-preview ' + shellQuote(source.url) + ' 500'));
  } finally { scope.stop(); }
});

test('preview setting persists, validates integers, falls back on corrupt/denied storage and is honest about save failure', () => {
  const values = new Map(), storage = { getItem: key => values.get(key), setItem: (key, value) => values.set(key, value) };
  const model = useUiPreferences(() => storage);
  assert.equal(model.characters.value, 500);
  for (const bad of ['99', '5001', '500.5', '', '1e3', 'NaN', '500;rm']) {
    model.draft.value = bad; assert.equal(model.save(), false); assert.equal(model.characters.value, 500);
  }
  model.draft.value = '1000'; assert.equal(model.save(), true);
  assert.equal(useUiPreferences(() => storage).characters.value, 1000);
  storage.getItem = () => 'invalid'; assert.equal(useUiPreferences(() => storage).characters.value, 500);
  const denied = useUiPreferences(() => { throw new Error('denied'); });
  denied.draft.value = '700'; assert.equal(denied.save(), true);
  assert.equal(denied.characters.value, 700); assert.match(denied.message.value, /无法持久保存/);
});

test('preview cache is scoped by count, expires, has bounded LRU and refresh failure does not disguise stale success', async () => {
  const scope = effectScope(), count = ref(100), calls = [];
  let time = 1000000, failed = false;
  const model = scope.run(() => useSourcePreview({ command: async command => {
    calls.push(command);
    const url = command.match(/--source-preview '([^']+)'/)[1];
    return failed ? { code: 1, stdout: JSON.stringify({ status: 'error', previews: [{ url, text: 'stale' }] }), stderr: 'process failed' }
      : ok([{ url, text: '中😀'.repeat(3000) }]);
  } }, count, () => Promise.resolve(), () => time));
  const ordinary = { url: 'https://example.org/cache' };
  try {
    await model.open(ordinary); assert.equal(Array.from(model.active.value.items[0].text).length, 100);
    count.value = 5000; await model.open(ordinary);
    assert.equal(Array.from(model.active.value.items[0].text).length, 5000); assert.equal(calls.length, 2);
    time += 300001; await model.open(ordinary); assert.equal(calls.length, 3);
    for (let i = 0; i < 16; i++) await model.open({ url: `https://example.org/lru${i}` });
    await model.open(ordinary); assert.equal(calls.length, 20);
    failed = true; await model.refresh(); assert.match(model.active.value.items[0].error, /process failed/);
    await model.open(ordinary); assert.equal(calls.length, 22); assert.match(model.active.value.items[0].error, /process failed/);
  } finally { scope.stop(); }
});

test('builtin dependencies run in parallel, independently show partial errors, do not mutate sources, and bound concurrency', async () => {
  const scope = effectScope(), tasks = [], builtin = { url: HAGEZI_ID, enabled: false, locked: true,
    dependencies: ['https://example.org/pro', 'https://example.org/fake'] };
  const model = scope.run(() => useSourcePreview({ command: command => new Promise(resolve => tasks.push({ command, resolve })) }));
  try {
    const loading = model.open(builtin); await Promise.resolve();
    assert.equal(tasks.length, 2);
    tasks[0].resolve(ok([{ url: builtin.dependencies[0], text: '<script>not HTML</script>' }]));
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(model.active.value.items[0].pending, false); assert.equal(model.active.value.items[1].pending, true);
    tasks[1].resolve(ok([{ url: builtin.dependencies[1], error: 'TLS verification failed' }]));
    await loading;
    assert.match(model.active.value.items[1].error, /TLS/); assert.equal(builtin.enabled, false); assert.equal(builtin.locked, true);
  } finally { scope.stop(); }
});

test('preview deduplicates in-flight reopen, blocks invalid protocols, rejects safely and discards late/disposed results', async () => {
  const scope = effectScope(), calls = [];
  let resolve;
  const model = scope.run(() => useSourcePreview({ command: command => { calls.push(command); return new Promise(done => { resolve = done; }); } }));
  await model.open({ url: 'file:///etc/passwd' }); assert.equal(calls.length, 0);
  assert.ok(model.active.value.items[0].error);
  const loading = model.open(source); await Promise.resolve();
  model.close(); const reopened = model.open(source); await Promise.resolve(); assert.equal(calls.length, 1);
  scope.stop(); resolve(ok([{ url: source.url, text: 'late' }])); await Promise.all([loading, reopened]);
  assert.equal(model.active.value, null); assert.equal(model.pending.value, false);
  const otherScope = effectScope();
  const failed = otherScope.run(() => useSourcePreview({ command: async () => { throw new Error('bridge unavailable'); } }));
  try { await failed.open(source); assert.match(failed.active.value.items[0].error, /bridge unavailable/); }
  finally { otherScope.stop(); }
});

test('two preview workers bound cross-source concurrency and closing cancels queued work', async () => {
  const scope = effectScope(), jobs = [];
  const model = scope.run(() => useSourcePreview({ command: command => new Promise(resolve => jobs.push({ command, resolve })) }));
  const builtin = { url: HAGEZI_ID, dependencies: ['https://example.org/pro', 'https://example.org/fake'] };
  try {
    const pair = model.open(builtin); await Promise.resolve(); assert.equal(jobs.length, 2);
    const queued = model.open({ url: 'https://example.org/queued' }); await Promise.resolve();
    assert.equal(jobs.length, 2, 'third source must wait for a worker');
    model.close(); await queued;
    jobs[0].resolve(ok([{ url: builtin.dependencies[0], text: 'pro' }]));
    jobs[1].resolve(ok([{ url: builtin.dependencies[1], text: 'fake' }]));
    await pair;
    assert.equal(jobs.length, 2, 'closed queued source must not start');
    assert.equal(model.active.value, null);
  } finally { scope.stop(); }
});
