import { computed, onScopeDispose, ref } from 'vue';
import { committedSourceOrder, HAGEZI_ID, lastJson, normalizeSource, parseSourceSummary, sourceSnapshot, sourcesDiffer, sourceUrlError, validJob } from '../domain/rules.js';
import { discoverJobs, observeJob } from '../domain/jobs.js';
import { PATHS } from '../services/api.js';
import { shellQuote } from '../services/bridge.js';
import { getStorage } from '../services/storage.js';
import { useLifetime } from './useLifetime.js';

export function useSources(api, feedback, storage = getStorage()) {
  const life = useLifetime();
  const rows = ref([]), saved = ref([]), order = ref([]), history = ref([]), ready = ref(false), saving = ref(false);
  const testingId = ref(null), testLogId = ref(null), testLog = ref(''), message = ref('正在读取来源…'), kind = ref('');
  const testPaused = ref(false), orphanTest = ref(''), recoveringTest = ref(false);
  let testTracking, rememberedTest;
  const editor = ref(null), editorError = ref('');
  let sequence = 0;
  const dirty = computed(() => ready.value && sourcesDiffer(rows.value, saved.value));
  const testing = computed(() => testingId.value !== null);
  const displayRows = computed(() => order.value.map(id => rows.value.find(s => s.id === id)).filter(Boolean));
  const enabledCount = computed(() => rows.value.filter(s => s.enabled).length);
  const ordinaryCount = computed(() => rows.value.filter(s => !s.locked).length);
  function replace(data) {
    rows.value = data.map(s => normalizeSource(s, sequence++));
    saved.value = sourceSnapshot(rows.value);
    order.value = committedSourceOrder(rows.value);
  }
  const remember = (key, value) => { try { value === null ? storage?.removeItem(key) : storage?.setItem(key, value); } catch { /* storage can be denied by WebView */ } };
  function status(text, statusKind = '') { message.value = text; kind.value = statusKind; }
  async function load() {
    const result = await api.command('--sources-list'), data = lastJson(result.stdout);
    if (!life.active) return;
    if (result.code || data?.status !== 'ok' || !Array.isArray(data.sources)) { status('来源读取失败，请确认已升级模块后重新打开页面。', 'error'); return; }
    replace(data.sources); ready.value = true; status('来源已读取；修改后请保存。');
    history.value = parseSourceSummary(await api.readFile(PATHS.sources))?.sources || [];
    await resumeTest();
  }
  function changed() { status(dirty.value ? '有未保存的修改。保存后，下次同步才会生效。' : '无未保存的修改。'); }
  function toggle(row, enabled) {
    if (saving.value || testingId.value === row.id) return;
    const wasDirty = dirty.value; row.enabled = enabled; changed();
    if (!wasDirty && dirty.value) feedback.toast('来源有修改，请点击「保存来源」，再到概览同步重建规则');
  }
  function edit(row = null) {
    if (!ready.value || saving.value || row?.locked || (row && testingId.value === row.id)) return;
    editor.value = { id: row?.id ?? null, url: row?.url || '' }; editorError.value = '';
  }
  function applyEdit() {
    if (!editor.value || saving.value) return false;
    const row = rows.value.find(s => s.id === editor.value.id);
    if (row?.locked || (row && testingId.value === row.id)) return false;
    const url = editor.value.url.trim(), error = sourceUrlError(url, rows.value, row?.id);
    if (error || (!row && (ordinaryCount.value >= 64 || url === HAGEZI_ID))) { editorError.value = error || '无法添加此来源'; return false; }
    if (row) row.url = url;
    else { const next = normalizeSource({ url, enabled: true }, sequence++); rows.value.push(next); order.value.push(next.id); }
    editor.value = null; changed(); return true;
  }
  function remove(row) {
    if (row.locked || saving.value || testingId.value === row.id) return;
    rows.value = rows.value.filter(s => s.id !== row.id); order.value = order.value.filter(id => id !== row.id);
    if (editor.value?.id === row.id) editor.value = null;
    changed();
  }
  async function save() {
    if (!ready.value || saving.value || testing.value || !dirty.value) return;
    for (const row of rows.value) {
      const error = sourceUrlError(row.url, rows.value, row.id);
      if (error) { status(error, 'error'); return; }
    }
    saving.value = true; status('正在校验并保存来源…');
    try {
      const text = rows.value.map(s => (s.enabled ? '' : '# disabled: ') + s.url).join('\n') + '\n';
      const result = await api.saveSources(text), data = lastJson(result.stdout);
      if (!life.active) return;
      if (!result.code && data?.status === 'ok') {
        // Preserve keyed DOM identity and focus if the backend returns the same
        // records; a successful save is the only draft-time reorder point.
        if (Array.isArray(data.sources)) {
          rows.value = data.sources.map(s => normalizeSource(s, rows.value.find(row => row.url === s.url)?.id ?? sequence++));
        }
        saved.value = sourceSnapshot(rows.value); order.value = committedSourceOrder(rows.value);
        editor.value = null; status('已保存。请到概览同步规则；当前引擎状态未改变。', 'success'); feedback.toast('来源已保存');
      } else status('保存失败：' + (result.stderr.trim() || '请检查后台任务是否正在运行'), 'error');
    } finally { saving.value = false; }
  }
  async function test(row) {
    if (saving.value || (testing.value && !testPaused.value)) return;
    if (testPaused.value) return resumeTest();
    const error = sourceUrlError(row.url, rows.value, row.id);
    if (error) { status(error, 'error'); return; }
    editor.value = null; testingId.value = row.id; testLogId.value = row.id; testLog.value = ''; orphanTest.value = ''; status('正在核实后台任务…');
    try {
      await life.wait(32);
      if (!life.active) return;
      const found = await discoverJobs(api, life, ['rules', 'source']);
      if (!life.active || found.cancelled) return;
      if (found.unknown) { pauseTest('后台任务未能核实，请点击“继续查询测试”；不会盲目创建任务。'); return; }
      if (found.job && ['running', 'unverified', 'invalid_state', 'not_found'].includes(found.job.status)) {
        if (found.job.type === 'source') return await attachTest(found.job);
        pauseTest('规则同步仍在后台运行，请点击“继续查询测试”核实后再测试来源。'); return;
      }
      const result = await api.command('--source-test ' + shellQuote(row.url)), data = lastJson(result.stdout);
      if (!life.active) return;
      if (result.code || data?.status !== 'started' || !validJob(data.job_id)) { pauseTest('测试派发结果未确认，请继续查询后端任务，勿重复创建。'); return; }
      rememberedTest = { id: data.job_id, url: row.url };
      remember('ad-killer-by-nfqueue-source-test', JSON.stringify(rememberedTest));
      await trackTest(data.job_id, row);
    } catch { if (life.active) pauseTest('测试结果未确认，请继续查询。'); }
  }
  function pauseTest(text) { testPaused.value = true; testingId.value ??= 'unverified'; status(text, 'warning'); }
  async function attachTest(job) {
    const row = rows.value.find(s => s.url === job.target) || { id: 'recovered-' + job.job_id, url: job.target };
    orphanTest.value = rows.value.includes(row) ? '' : job.target;
    rememberedTest = { id: job.job_id, url: job.target };
    remember('ad-killer-by-nfqueue-source-test', JSON.stringify(rememberedTest));
    return trackTest(job.job_id, row);
  }
  async function resumeTest() {
    if (testTracking || saving.value || recoveringTest.value) return;
    recoveringTest.value = true;
    try {
      let known = rememberedTest;
      try { known ||= JSON.parse(storage?.getItem('ad-killer-by-nfqueue-source-test') || 'null'); } catch { /* corrupt storage */ }
      const found = await discoverJobs(api, life, ['rules', 'source']);
      if (!life.active || found.cancelled) return;
      if (found.job?.type === 'source') { void attachTest(found.job); return; }
      if (found.job?.type === 'rules' && ['running', 'unverified', 'invalid_state', 'not_found'].includes(found.job.status)) {
        pauseTest('后台规则同步尚未结束，请继续查询核实；不会启动单源测试。'); return;
      }
      if (validJob(known?.id)) { void attachTest({ job_id: known.id, target: known.url || '已移除来源' }); return; }
      if (found.unknown) { pauseTest('后台任务索引未能核实，请继续查询测试。'); return; }
      testPaused.value = false; testingId.value = null; rememberedTest = null;
      remember('ad-killer-by-nfqueue-source-test', null);
      status('未发现活动来源测试；可选择来源进行测试。');
    } catch { if (life.active) pauseTest('测试恢复状态未能核实，请继续查询。'); }
    finally { recoveringTest.value = false; }
  }
  async function trackTest(id, row) {
    if (testTracking) return testTracking;
    testingId.value = row.id; testLogId.value = row.id; testPaused.value = false;
    status(`正在测试：${row.url}（不替换当前规则）`);
    testTracking = (async () => {
      const outcome = await observeJob(api, id, life, {
        types: ['source'],
        onLog: (_, text) => { testLog.value = text; },
        onRetry: () => status('测试状态暂未核实，正在退避重试；后台任务不会被停止。', 'warning'),
      });
      if (!life.active || outcome.kind === 'cancelled') return;
      if (outcome.kind === 'unknown') { pauseTest('测试结果仍未确认，已暂停自动查询。可点击“继续查询测试”。'); return; }
      if (outcome.kind === 'unavailable') {
        rememberedTest = null; remember('ad-killer-by-nfqueue-source-test', null);
        pauseTest('来源测试任务记录不可用，请继续查询后端索引；当前规则未改变。'); return;
      }
      const ok = outcome.state.status === 'success' && outcome.state.exit_code === 0;
      status((ok ? '来源测试通过；未替换规则。有效行数与过滤数量见测试日志。' : '来源测试失败或中断；当前规则未改变。原因见测试日志。') + (outcome.logError ? ' 日志片段未能完整读取。' : ''), ok ? 'success' : 'error');
      testingId.value = null; testPaused.value = false; rememberedTest = null;
      remember('ad-killer-by-nfqueue-source-test', null);
    })();
    try { await testTracking; }
    catch { if (life.active) pauseTest('测试观察结果未确认，请继续查询。'); }
    finally { testTracking = null; }
  }
  const beforeUnload = event => { if (dirty.value) { event.preventDefault(); event.returnValue = ''; } };
  globalThis.window?.addEventListener('beforeunload', beforeUnload);
  onScopeDispose(() => globalThis.window?.removeEventListener('beforeunload', beforeUnload));
  return { rows, displayRows, history, ready, saving, dirty, testing, testingId, testLogId, testLog, testPaused, orphanTest, recoveringTest, message, kind,
    editor, editorError, enabledCount, ordinaryCount, load, toggle, edit, applyEdit, remove, save, test, resumeTest };
}
