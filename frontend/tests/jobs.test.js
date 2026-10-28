import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, ref } from 'vue';
import { createJobLogReader, discoverJobs, observeJob } from '../src/domain/jobs.js';
import { useSync } from '../src/composables/useSync.js';
import { useSources } from '../src/composables/useSources.js';
import { useModuleUpdate } from '../src/composables/useModuleUpdate.js';

const id = '123-456';
const ok = value => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
const page = (bytes, extra = {}) => ok({ status: 'ok', data_base64: Buffer.from(bytes).toString('base64'), cursor: 'epoch:100', reset: false, more: false, omitted_bytes: 0, ...extra });
const lifetime = () => ({ active: true, waits: [], async wait(ms) { this.waits.push(ms); } });
const feedback = () => ({ log() {}, toast() {}, consoleOpen: ref(false) });
const denied = { getItem() { throw Error('denied'); }, setItem() { throw Error('denied'); }, removeItem() { throw Error('denied'); } };
const settle = async (condition, attempts = 100) => { for (let i = 0; i < attempts && !condition(); ++i) await new Promise(resolve => setTimeout(resolve, 1)); assert.ok(condition()); };

test('job logs stream split UTF-8, bound display tails, reset epochs and reject bad chunk schemas', async () => {
  const bytes = new TextEncoder().encode('中文😀'), calls = [];
  const replies = [page(bytes.slice(0, 2), { reset: true, reset_reason: 'begin', cursor: 'epoch:2' }), page(bytes.slice(2)), page(new TextEncoder().encode('new'), { reset: true, reset_reason: 'rotated', cursor: 'next:3' })];
  const reader = createJobLogReader({ readJobChunk: async (_, cursor) => { calls.push(cursor); return replies.shift(); } }, id);
  assert.equal((await reader.read()).delta, '');
  assert.equal((await reader.read()).delta, '中文😀'); assert.equal(reader.text, '中文😀');
  await reader.read(); assert.equal(reader.text, 'new'); assert.deepEqual(calls, ['', 'epoch:2', 'epoch:100']);
  const large = createJobLogReader({ readJobChunk: async () => page(new Uint8Array(16384).fill(97)) }, id);
  for (let i = 0; i < 8; ++i) await large.read();
  assert.equal(large.text.length, 65536);
  for (const bad of [{ cursor: '../bad' }, { data_base64: '!!!' }, { omitted_bytes: -1 }, { data_base64: Buffer.alloc(20000).toString('base64') }]) {
    const rejected = createJobLogReader({ readJobChunk: async () => page([], bad) }, id);
    await assert.rejects(rejected.read(), /片段/);
  }
});

test('query failures use bounded exponential backoff and never dispatch mutations or logs on an unverified identity', async () => {
  const life = lifetime(), commands = [];
  const result = await observeJob({ command: async cmd => { commands.push(cmd); return { code: 1, stdout: '{"status":"unverified"}' }; }, readJobChunk: async () => { throw Error('must not read logs'); } }, id, life);
  assert.equal(result.kind, 'unknown'); assert.equal(commands.length, 5);
  assert.deepEqual(life.waits, [1000, 2000, 4000, 8000]);
  assert.ok(commands.every(cmd => cmd === '--update-status ' + id));
  const wrong = await observeJob({ command: async () => ok({ status: 'success', exit_code: 0, job_id: '999-111' }), readJobChunk: async () => page([]) }, id, lifetime(), { interval: 0 });
  assert.equal(wrong.kind, 'unknown');
});

test('background visibility/disposal pause observations without cancelling workers or publishing late feedback', async () => {
  const life = lifetime(), document = { hidden: true }; let calls = 0;
  life.wait = async () => { document.hidden = false; };
  const result = await observeJob({ command: async () => { calls++; return ok({ status: 'success', exit_code: 0, job_id: id }); }, readJobChunk: async () => page([]) }, id, life, { document });
  assert.equal(result.kind, 'terminal'); assert.equal(calls, 1);
  const closed = lifetime(); let chunks = 0, published = 0;
  const disposed = await observeJob({ command: async () => { closed.active = false; return ok({ status: 'running', job_id: id }); }, readJobChunk: async () => { chunks++; return page([]); } }, id, closed, { onState() { published++; } });
  assert.equal(disposed.kind, 'cancelled'); assert.equal(chunks, 0); assert.equal(published, 0);
  await discoverJobs({ command: async () => { calls++; throw Error('closed'); } }, closed, ['rules']);
  assert.equal(calls, 1, 'discovery never starts after disposal');
  const closedInLog = lifetime(); let logReads = 0;
  const noTail = await observeJob({ command: async () => ok({ status: 'success', exit_code: 0, job_id: id }),
    readJobChunk: async () => { logReads++; return page([97], { more: true }); } }, id, closedInLog,
  { onLog() { closedInLog.active = false; } });
  assert.equal(noTail.kind, 'cancelled'); assert.equal(logReads, 1, 'closing after a chunk cannot dispatch a hidden tail read');
});

test('terminal log backlog uses a bounded explicit tail seek and never changes task success into a log failure', async () => {
  const cursors = [], logs = [];
  const result = await observeJob({ command: async () => ok({ status: 'success', exit_code: 0, job_id: id }), readJobChunk: async (_, cursor) => {
    cursors.push(cursor); return cursor === 'tail' ? page(Buffer.from('final'), { reset: true, reset_reason: 'tail', omitted_bytes: 999 }) : page(Buffer.from('first'), { more: true });
  } }, id, lifetime(), { onLog: chunk => logs.push(chunk) });
  assert.deepEqual(cursors, ['', 'tail']); assert.equal(result.kind, 'terminal'); assert.equal(result.text, 'final'); assert.equal(logs[1].omitted, 999);
  const noLog = await observeJob({ command: async () => ok({ status: 'success', exit_code: 0, job_id: id }), readJobChunk: async () => { throw Error('log unavailable'); } }, id, lifetime());
  assert.equal(noLog.kind, 'terminal'); assert.equal(noLog.logError, true);
});

test('discovery refuses malformed/unknown task indexes instead of pretending the backend is empty', async () => {
  for (const value of [{ status: 'unverified' }, { status: 'ok', jobs: [{ job_id: '../bad', type: 'rules' }] }, { status: 'ok', jobs: [{ job_id: id, type: '' }] }])
    assert.equal((await discoverJobs({ command: async () => ok(value) }, lifetime(), ['rules'])).unknown, true);
  assert.equal((await discoverJobs({ command: async () => ok({ status: 'ok', jobs: [] }) }, lifetime(), ['rules'])).job, null);
});

test('rule synchronization resumes a backend-owned job without localStorage and never creates another', async () => {
  const scope = effectScope(), commands = [];
  const api = { command: async cmd => {
    commands.push(cmd); return cmd === '--jobs-list' ? ok({ status: 'ok', jobs: [{ job_id: id, type: 'rules', status: 'running' }] }) : ok({ status: 'success', exit_code: 0, job_id: id, type: 'rules' });
  }, readJobChunk: async () => page([]), readFile: async () => '' };
  const model = scope.run(() => useSync(api, feedback(), ref([]), async () => {}, denied));
  try {
    await model.resume(); await settle(() => !model.busy.value);
    assert.equal(model.kind.value, 'success'); assert.ok(!commands.includes('--update-start'));
    assert.deepEqual(commands, ['--jobs-list', '--update-status ' + id]);
  } finally { scope.stop(); }
});

test('an unsaved source test can be recovered separately without mutating configured sources', async () => {
  const scope = effectScope(), commands = [], target = 'https://unsaved.example.org/rules';
  const api = { command: async cmd => {
    commands.push(cmd);
    if (cmd === '--sources-list') return ok({ status: 'ok', sources: [{ url: 'https://saved.example.org', enabled: true }] });
    if (cmd === '--jobs-list') return ok({ status: 'ok', jobs: [{ job_id: id, type: 'source', target, status: 'running' }] });
    return ok({ status: 'success', exit_code: 0, job_id: id, type: 'source' });
  }, readFile: async () => '', readJobChunk: async () => page(Buffer.from('<img onerror=alert(1)>中文')) };
  const model = scope.run(() => useSources(api, feedback(), denied));
  try {
    await model.load(); await settle(() => !model.testing.value);
    assert.equal(model.orphanTest.value, target); assert.equal(model.rows.value.length, 1); assert.equal(model.dirty.value, false);
    assert.equal(model.testLog.value, '<img onerror=alert(1)>中文'); assert.ok(!commands.some(cmd => cmd.startsWith('--source-test')));
  } finally { scope.stop(); }
});

test('unknown indexes keep mutation paused; an explicit retry only discovers and never resubmits', async () => {
  const scope = effectScope(), commands = [];
  const api = { command: async cmd => { commands.push(cmd); return { code: 1, stdout: '{"status":"unverified"}' }; }, readFile: async () => '' };
  const model = scope.run(() => useSync(api, feedback(), ref([]), async () => {}, denied));
  try {
    await model.start(); assert.equal(model.uncertain.value, true); assert.equal(model.busy.value, false);
    await model.start(); assert.equal(model.uncertain.value, true);
    assert.deepEqual(commands, ['--jobs-list', '--jobs-list']);
  } finally { scope.stop(); }
});

test('module downloads recover from backend identity with denied storage, without downloading or installing again', async () => {
  const scope = effectScope(), commands = [];
  const api = { command: async cmd => {
    commands.push(cmd);
    if (cmd === '--module-update-status') return ok({ version: '1.3', channel: 'stable', rebootRequired: false });
    if (cmd === '--jobs-list') return ok({ status: 'ok', jobs: [{ job_id: id, type: 'download', target: '1.3', channel: 'stable', status: 'running' }] });
    return ok({ status: 'success', exit_code: 0, job_id: id, type: 'download' });
  }, readJobChunk: async () => page([]) };
  const info = { verified: ref(true), mismatch: ref(false), version: ref('1.2'), releaseChannel: ref('stable') };
  const model = scope.run(() => useModuleUpdate(api, info, feedback(), denied, 1));
  try {
    await model.resume(); assert.equal(model.canInstall.value, true); assert.equal(model.ready.value, '1.3');
    assert.ok(commands.every(cmd => !/--module-update-(download|install) /.test(cmd)));
  } finally { scope.stop(); }
});

test('historical completed synchronization stays silent across WebUI remounts', async () => {
  for (const [status, exit_code] of [['success', 0], ['failed', 1], ['interrupted', 255]]) {
    for (let opened = 0; opened < 2; opened++) {
      const scope = effectScope(), commands = [], notices = [], history = ref([]);
      const ui = { ...feedback(), toast: text => notices.push(text) };
      const job = { status, exit_code, job_id: id, type: 'rules' };
      const api = {
        command: async cmd => { commands.push(cmd); return ok(cmd === '--jobs-list' ? { status: 'ok', jobs: [job] } : job); },
        readJobChunk: async () => page([]),
        readFile: async () => `job_id=${id}\npublish_status=published\ngeneration=999-111\nsource_count=1\naccepted_sources=1\n\n[source_0]\nurl=https://example.org/list\nstatus=accepted\n`,
      };
      const model = scope.run(() => useSync(api, ui, history, async () => {}, denied));
      try {
        await model.resume(); await settle(() => !model.busy.value);
        assert.deepEqual(notices, [], 'opening must not announce a previously completed task');
        assert.equal(ui.consoleOpen.value, false, 'old failures must not reopen the console');
        assert.match(model.message.value, /上次同步/);
        assert.equal(history.value.length, 1, 'historical source results remain available');
        assert.equal(commands.includes('--update-start'), false);
      } finally { scope.stop(); }
    }
  }
});

test('completed job found only in saved storage is recovered without a completion toast', async () => {
  const scope = effectScope(), notices = [], commands = []; let stored = id;
  const storage = { getItem() { return stored; }, setItem(_, value) { stored = value; }, removeItem() { stored = null; } };
  const api = {
    command: async cmd => { commands.push(cmd); return ok(cmd === '--jobs-list' ? { status: 'ok', jobs: [] } : { status: 'success', exit_code: 0, job_id: id, type: 'rules' }); },
    readJobChunk: async () => page([]), readFile: async () => '',
  };
  const model = scope.run(() => useSync(api, { ...feedback(), toast: text => notices.push(text) }, ref([]), async () => {}, storage));
  try {
    await model.resume(); await settle(() => !model.busy.value);
    assert.deepEqual(notices, []);
    assert.equal(stored, null);
    assert.equal(commands.includes('--update-start'), false);
    assert.equal(model.kind.value, 'success');
  } finally { scope.stop(); }
});

test('manual synchronization completion is announced once and later history reads are silent', async () => {
  const scope = effectScope(), notices = [], commands = []; let starts = 0;
  let job = { status: 'success', exit_code: 0, job_id: id, type: 'rules' };
  const api = {
    command: async cmd => {
      commands.push(cmd);
      if (cmd === '--jobs-list') return ok({ status: 'ok', jobs: [job] });
      if (cmd === '--update-start') {
        job = { ...job, job_id: `123-${457 + starts++}` };
        return ok({ status: 'started', job_id: job.job_id });
      }
      return ok(job);
    },
    readJobChunk: async () => page([]), readFile: async () => '',
  };
  const model = scope.run(() => useSync(api, { ...feedback(), toast: text => notices.push(text) }, ref([]), async () => {}, denied));
  try {
    await model.start();
    assert.deepEqual(notices, ['规则已同步完成']);
    await model.resume(); await settle(() => !model.busy.value);
    assert.deepEqual(notices, ['规则已同步完成']);
    await model.start();
    assert.deepEqual(notices, ['规则已同步完成', '规则已同步完成']);
    assert.equal(starts, 2);
  } finally { scope.stop(); }
});

test('recovering a genuinely running synchronization still announces its completion', async () => {
  const scope = effectScope(), notices = [];
  const api = {
    command: async cmd => ok(cmd === '--jobs-list'
      ? { status: 'ok', jobs: [{ job_id: id, type: 'rules', status: 'running' }] }
      : { status: 'success', exit_code: 0, job_id: id, type: 'rules' }),
    readJobChunk: async () => page([]), readFile: async () => '',
  };
  const model = scope.run(() => useSync(api, { ...feedback(), toast: text => notices.push(text) }, ref([]), async () => {}, denied));
  try {
    await model.resume(); await settle(() => !model.busy.value);
    assert.deepEqual(notices, ['规则已同步完成']);
  } finally { scope.stop(); }
});

test('an unfinished manual result keeps its notification when explicit recovery finishes', async () => {
  const scope = effectScope(), notices = []; let started = false, reads = 0;
  const job = { status: 'success', exit_code: 0, job_id: id, type: 'rules' };
  const api = {
    command: async cmd => {
      if (cmd === '--jobs-list') return ok({ status: 'ok', jobs: started ? [job] : [] });
      if (cmd === '--update-start') { started = true; return ok({ status: 'started', job_id: id }); }
      return ok(job);
    },
    readJobChunk: async () => page([]),
    readFile: async () => { if (++reads === 1) throw Error('bridge read failed'); return ''; },
  };
  const model = scope.run(() => useSync(api, { ...feedback(), toast: text => notices.push(text) }, ref([]), async () => {}, denied));
  try {
    await model.start(); assert.equal(model.uncertain.value, true); assert.deepEqual(notices, []);
    await model.resume(); await settle(() => !model.busy.value);
    assert.deepEqual(notices, ['规则已同步完成']);
    await model.resume(); await settle(() => !model.busy.value);
    assert.deepEqual(notices, ['规则已同步完成']);
  } finally { scope.stop(); }
});

test('a saved job observed running after an empty index still announces completion', async () => {
  const scope = effectScope(), notices = []; let polls = 0;
  const api = {
    command: async cmd => ok(cmd === '--jobs-list' ? { status: 'ok', jobs: [] }
      : { job_id: id, type: 'rules', status: ++polls === 1 ? 'running' : 'success', exit_code: 0 }),
    readJobChunk: async () => page([]), readFile: async () => '',
  };
  const storage = { getItem() { return id; }, setItem() {}, removeItem() {} };
  const model = scope.run(() => useSync(api, { ...feedback(), toast: text => notices.push(text) }, ref([]), async () => {}, storage));
  try {
    await model.resume(); await settle(() => polls === 1);
    assert.deepEqual(notices, []);
    await settle(() => !model.busy.value, 2000);
    assert.equal(polls, 2);
    assert.deepEqual(notices, ['规则已同步完成']);
  } finally { scope.stop(); }
});
