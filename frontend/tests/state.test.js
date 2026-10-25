import assert from 'node:assert/strict';
import test from 'node:test';
import { effectScope, nextTick, ref } from 'vue';
import { useSources } from '../src/composables/useSources.js';
import { useSync } from '../src/composables/useSync.js';
import { useEngine, waitEngine } from '../src/composables/useEngine.js';
import { useApps } from '../src/composables/useApps.js';
import { useCustomRules } from '../src/composables/useCustomRules.js';
import { useFeedback } from '../src/composables/useFeedback.js';
import { HAGEZI_ID } from '../src/domain/rules.js';
import { PATHS } from '../src/services/api.js';
import { applied } from './fixtures/apply.js';

const ok = (value = {}) => ({ code: 0, stdout: JSON.stringify(value), stderr: '' });
const feedback = () => ({ log() {}, toast() {}, consoleOpen: ref(false) });
const memory = () => { const entries = new Map(); return { getItem: k => entries.get(k), setItem: (k, v) => entries.set(k, v), removeItem: k => entries.delete(k) }; };

test('source CRUD, immutable builtin, stable toggle/test order, failed and successful save', async () => {
  const scope = effectScope(), commands = [], storage = memory();
  let saveFails = true, submitted;
  const fixture = [{ url: 'https://off.org', enabled: false }, { url: 'https://on.org', enabled: true }, { url: HAGEZI_ID, enabled: false }];
  const api = {
    command: async args => { commands.push(args); return args === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : args === '--sources-list' ? ok({ status: 'ok', sources: fixture }) : args.startsWith('--source-test') ? ok({ status: 'started', job_id: '123-456' }) : ok({ status: 'success', exit_code: 0 }); },
    readFile: async () => '', readJob: async () => 'test only',
    saveSources: async text => { submitted = text; return saveFails ? { code: 2, stdout: '', stderr: 'busy' } : ok({ status: 'ok' }); },
  };
  const sources = scope.run(() => useSources(api, feedback(), storage));
  try {
    await sources.load(); assert.deepEqual(sources.displayRows.value.map(r => r.id), [1, 0, 2]);
    sources.toggle(sources.rows.value[1], false); assert.equal(sources.dirty.value, true);
    assert.deepEqual(sources.displayRows.value.map(r => r.id), [1, 0, 2]);
    await sources.save(); assert.match(sources.message.value, /保存失败/); assert.equal(sources.dirty.value, true);
    assert.deepEqual(sources.displayRows.value.map(r => r.id), [1, 0, 2]);
    sources.toggle(sources.rows.value[0], true);
    const builtin = sources.rows.value[2];
    sources.edit(builtin); assert.equal(sources.editor.value, null);
    sources.remove(builtin); assert.equal(sources.rows.value.length, 3);
    await sources.test(builtin); assert.match(sources.message.value, /测试通过/);
    assert.equal(sources.history.value.length, 0); assert.equal(storage.getItem('ad-killer-by-nfqueue-source-test'), undefined);
    saveFails = false; await sources.save();
    assert.equal(sources.dirty.value, false); assert.deepEqual(sources.displayRows.value.map(r => r.id), [0, 1, 2]);
    assert.match(submitted, /# disabled: builtin:hagezi-pro-minus-fake/);
    sources.edit(); sources.editor.value.url = 'https://new.org'; assert.equal(sources.applyEdit(), true);
    assert.equal(sources.rows.value.length, 4); assert.equal(sources.displayRows.value.at(-1).url, 'https://new.org');
    assert.equal(commands.filter(c => /--(?:start|stop|daemon|reload)/.test(c)).length, 0);
  } finally { scope.stop(); }
});

test('dispose source observer leaves resumable job and prevents delayed status publication', async () => {
  const scope = effectScope(), storage = memory();
  storage.setItem('ad-killer-by-nfqueue-source-test', JSON.stringify({ id: '123-456', url: HAGEZI_ID }));
  const api = { command: async args => args === '--sources-list' ? ok({ status: 'ok', sources: [{ url: HAGEZI_ID }] }) : ok({ status: 'running' }), readFile: async () => '', readJob: async () => 'still running' };
  const sources = scope.run(() => useSources(api, feedback(), storage));
  await sources.load(); await nextTick(); scope.stop();
  assert.ok(storage.getItem('ad-killer-by-nfqueue-source-test')); assert.equal(sources.history.value.length, 0);
});

test('engine waits distinguish error, query, early exit, bad route and actual timeout', async () => {
  let state = { runtime: 'error', running: false, last_error: 'NFQUEUE occupied' }, polls = 0;
  const api = { command: async () => { polls++; return ok(state); } }, life = { active: true, wait: async () => {} };
  const refresh = async () => ({ running: true, iptables_active: true, apply: applied() });
  assert.match((await waitEngine('running', api, refresh, life)).message, /occupied/);
  state = { runtime: 'stopped', running: false }; assert.equal((await waitEngine('running', api, refresh, life)).kind, 'exited');
  state = { runtime: 'running', running: true }; assert.equal((await waitEngine('running', api, refresh, life)).ok, true);
  assert.equal((await waitEngine('running', api, async () => ({ running: true, iptables_active: false }), life)).ok, false);
  state = { runtime: 'starting', running: true }; polls = 0;
  assert.equal((await waitEngine('running', api, refresh, life)).kind, 'timeout'); assert.equal(polls, 40);
  assert.equal((await waitEngine('running', { command: async () => ({ code: 1 }) }, refresh, life)).kind, 'query');
});

test('autostart persists only boot policy, never starts/stops; all settings use CLI and refresh', async () => {
  const scope = effectScope(), commands = [];
  let state = { running: false, iptables_active: false, runtime: 'stopped', autostart: false, mode: 2, scope: 0 };
  const api = { readFile: async () => 'black=2\nwhite=1', command: async args => {
    commands.push(args);
    if (args === '--autostart on') { state = { ...state, autostart: true }; return ok({ autostart: true }); }
    if (args === '--autostart off') return { code: 1, stdout: '', stderr: 'denied' };
    if (args === '--scope 1') return ok({ status: 'deferred', revision: 'a'.repeat(64) });
    return ok(state);
  } };
  const engine = scope.run(() => useEngine(api, feedback()));
  try {
    await engine.refresh(); assert.equal(engine.total.value, 3); assert.equal(engine.kind.value, 'neutral');
    await engine.changeSetting('autostart', true); assert.equal(engine.state.value.autostart, true);
    await engine.changeSetting('autostart', false); assert.equal(engine.state.value.autostart, true);
    await engine.changeSetting('scope', 1); assert.ok(commands.includes('--scope 1'));
    assert.ok(!commands.includes('--apps-apply'), 'scope and UID mapping are one backend submission, not two competing reloads');
    assert.equal(commands.filter(c => /^--(?:start|stop|daemon)$/.test(c)).length, 0);
    assert.equal(engine.setting.value, '');
  } finally { scope.stop(); }
});

test('empty-rule startup explains synchronization first and never enters daemon polling', async () => {
  const scope=effectScope(), commands=[], notices=[];
  const state={running:false,iptables_active:false,runtime:'stopped',enabled:false,manual_stop:true,rules_black:0,rules_white:0};
  const api={readFile:async()=>'',command:async cmd=>{
    commands.push(cmd);
    return cmd==='--start'?{code:1,stdout:JSON.stringify({status:'error_rules_empty',message:'no rules'}),stderr:''}:ok(state);
  }};
  const model=scope.run(()=>useEngine(api,{log(){},toast:msg=>notices.push(msg),consoleOpen:ref(false)}));
  try {
    await model.refresh(); assert.equal(model.total.value,0); await model.control('start');
    assert.match(model.description.value,/先点击“同步开源规则”/);
    assert.match(notices[0],/同步成功后再启动/);
    assert.equal(model.running.value,false); assert.equal(model.action.value,'');
    assert.deepEqual(commands,['--status','--start','--status']);
  } finally {scope.stop();}
});

test('superseded lifecycle requests refresh actual state without polling or claiming stop/start success', async () => {
  for (const verb of ['start', 'stop']) {
    const scope = effectScope(), commands = [], messages = [];
    const api = { readFile: async () => '', command: async command => {
      commands.push(command);
      return command === `--${verb}` ? ok({ status: 'cancelled' }) : ok({ running: false, iptables_active: false, runtime: 'stopped', scope: 0 });
    } };
    const model = scope.run(() => useEngine(api, { log: text => messages.push(text), toast() {}, consoleOpen: ref(false) }));
    try {
      await model.refresh(); await model.control(verb);
      assert.deepEqual(commands, ['--status', `--${verb}`, '--status']);
      assert.ok(messages.some(message => message.includes('已取消')));
      assert.ok(!messages.some(message => message.includes('引擎与规则已')));
    } finally { scope.stop(); }
  }
});

test('scope-specific app lists and custom rules retain commands and content semantics', async () => {
  const scope = effectScope(), current = ref(0), writes = [], commands = [];
  const api = {
    readFile: async path => path === PATHS.allow ? 'com.example.allowed\n' : path === PATHS.intercept ? 'com.example.blocked\n' : 'ads.org\n@@safe.org\n',
    writeFile: async (path, text) => { writes.push({ path, text }); return ok(); },
    command: async args => { commands.push(args); return ok({ status: 'deferred' }); },
    packageManager: () => null,
  };
  const apps = scope.run(() => useApps(api, feedback(), current));
  const custom = scope.run(() => useCustomRules(api, feedback()));
  try {
    await apps.load(); assert.ok(apps.selected.value.has('com.example.allowed')); await apps.save();
    current.value = 1; await nextTick(); await apps.load(); await apps.save();
    assert.ok(apps.selected.value.has('com.example.blocked'));
    assert.equal(writes[0].path, PATHS.allow); assert.equal(writes[1].path, PATHS.intercept);
    await custom.load(); assert.equal(custom.black.value, 'ads.org'); assert.equal(custom.white.value, 'safe.org');
    custom.white.value += '\n@@other.org'; await custom.save();
    assert.equal(writes[2].text, 'ads.org\n@@safe.org\n@@other.org\n');
    assert.deepEqual(commands, ['--apps-apply', '--apps-apply', '--reload']);
  } finally { scope.stop(); }
});

test('unique update task publication, partial sources, resume, logs and observer cleanup', async () => {
  const scope = effectScope(), history = ref([]), storage = memory(), commands = [];
  let refreshed = 0;
  const api = {
    command: async args => { commands.push(args); return args === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : args === '--update-start' ? ok({ status: 'started', job_id: '123-456' }) : ok({ status: 'success', exit_code: 0 }); },
    readJob: async () => 'generation 999-111 已原子发布',
    readFile: async () => 'job_id=123-456\npublish_status=published\ngeneration=999-111\nsource_count=2\naccepted_sources=1\n\n[source_0]\nurl=https://a.org\nstatus=accepted\n',
  };
  const sync = scope.run(() => useSync(api, feedback(), history, async () => { refreshed++; }, storage));
  try {
    await sync.start(); assert.equal(sync.busy.value, false); assert.equal(sync.kind.value, 'warning');
    assert.match(sync.message.value, /部分来源失败/); assert.equal(history.value.length, 1); assert.equal(refreshed, 1);
    assert.equal(storage.getItem('ad-killer-by-nfqueue-update-job'), undefined);
    storage.setItem('ad-killer-by-nfqueue-update-job', '123-456'); await sync.resume(); await nextTick(); await nextTick();
    assert.equal(commands.filter(c => c === '--update-start').length, 1, 'resume must not create a second job');
  } finally { scope.stop(); }
});

test('complete cached-source publication warns about cache reuse without announcing a partial ruleset', async () => {
  const scope = effectScope(), history = ref([]);
  const api = {
    command: async args => args === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : args === '--update-start' ? ok({ status: 'started', job_id: '123-456' }) : ok({ status: 'success', exit_code: 0 }),
    readJob: async () => '整组复用缓存\ngeneration 999-111 已原子发布',
    readFile: async () => 'job_id=123-456\npublish_status=published\ngeneration=999-111\nsource_count=1\naccepted_sources=1\ncached_sources=1\n\n[source_0]\nurl=https://a.org\nstatus=accepted\nprovenance=cache_fallback\n',
  };
  const sync = scope.run(() => useSync(api, feedback(), history, async () => {}, memory()));
  try {
    await sync.start();
    assert.equal(sync.kind.value, 'warning'); assert.match(sync.message.value, /使用缓存/);
    assert.doesNotMatch(sync.message.value, /部分来源失败|仅合并/);
    assert.equal(history.value[0].provenance, 'cache_fallback');
  } finally { scope.stop(); }
});

test('diagnostic buffer is bounded and feedback treats hostile text as text', () => {
  const scope = effectScope(), model = scope.run(useFeedback);
  try { model.log(Array.from({ length: 501 }, (_, i) => `line ${i}`).join('\n')); assert.equal(model.logs.value.length, 400); assert.equal(model.logs.value[0].text, 'line 101'); }
  finally { scope.stop(); }
});

test('interrupted publishing is unknown, never a promise that the old pointer survived', async () => {
  const scope = effectScope();
  const api = { command: async command => command === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : command === '--update-start'
    ? ok({ status: 'started', job_id: '123-456' }) : ok({ status: 'interrupted', exit_code: 255 }),
  readFile: async () => '', readJob: async () => 'generation 已原子发布' };
  const sync = scope.run(() => useSync(api, feedback(), ref([]), async () => {}, memory()));
  try {
    await sync.start();
    assert.match(sync.message.value, /发布结果未确认/);
    assert.doesNotMatch(sync.message.value, /旧规则未被替换|当前规则未改变/);
    assert.equal(sync.busy.value, false);
  } finally { scope.stop(); }
});

test('WebView denied localStorage getter does not prevent source loading or synchronization', async () => {
  const previous = Object.getOwnPropertyDescriptor(globalThis, 'localStorage'), scope = effectScope();
  Object.defineProperty(globalThis, 'localStorage', { configurable: true, get() { throw new Error('SecurityError'); } });
  const api = {
    command: async args => args === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : args === '--sources-list' ? ok({ status: 'ok', sources: [] }) : args === '--update-start' ? ok({ status: 'started', job_id: '123-456' }) : ok({ status: 'success', exit_code: 0 }),
    readFile: async () => '', readJob: async () => '',
  };
  try {
    const sources = scope.run(() => useSources(api, feedback()));
    await sources.load(); assert.equal(sources.ready.value, true);
    const sync = scope.run(() => useSync(api, feedback(), ref([]), async () => {}));
    await sync.start(); assert.equal(sync.kind.value, 'success');
  } finally {
    scope.stop();
    if (previous) Object.defineProperty(globalThis, 'localStorage', previous); else delete globalThis.localStorage;
  }
});

test('missing persisted jobs pause mutation until explicit backend recovery, without an endless spinner', async () => {
  const scope = effectScope(), storage = memory();
  storage.setItem('ad-killer-by-nfqueue-update-job', '123-456');
  storage.setItem('ad-killer-by-nfqueue-source-test', JSON.stringify({ id: '987-654', url: HAGEZI_ID }));
  const api = {
    command: async args => args === '--jobs-list' ? ok({ status: 'ok', jobs: [] }) : args === '--sources-list' ? ok({ status: 'ok', sources: [{ url: HAGEZI_ID }] }) : { code: 1, stdout: '{"status":"not_found"}', stderr: '' },
    readFile: async () => '', readJob: async () => '',
  };
  const sources = scope.run(() => useSources(api, feedback(), storage));
  const sync = scope.run(() => useSync(api, feedback(), ref([]), async () => {}, storage));
  try {
    await sources.load(); await sync.resume();
    for (let i = 0; i < 10; i++) await nextTick();
    assert.equal(sources.testPaused.value, true); assert.equal(sources.kind.value, 'warning');
    assert.equal(sync.busy.value, false); assert.match(sync.message.value, /任务记录不存在/);
    assert.equal(storage.getItem('ad-killer-by-nfqueue-update-job'), undefined); assert.equal(storage.getItem('ad-killer-by-nfqueue-source-test'), undefined);
    assert.equal(sync.uncertain.value, true);
    await sources.resumeTest(); await sync.resume();
    assert.equal(sources.testing.value, false); assert.equal(sync.uncertain.value, false);
  } finally { scope.stop(); }
});
