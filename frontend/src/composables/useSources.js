import { computed, onScopeDispose, ref } from 'vue';
import { committedSourceOrder, HAGEZI_ID, jobObservation, lastJson, normalizeSource, parseSourceSummary, sourceSnapshot, sourcesDiffer, sourceUrlError, validJob } from '../domain/rules.js';
import { PATHS } from '../services/api.js';
import { shellQuote } from '../services/bridge.js';
import { getStorage } from '../services/storage.js';
import { useLifetime } from './useLifetime.js';

export function useSources(api, feedback, storage = getStorage()) {
  const life = useLifetime();
  const rows = ref([]), saved = ref([]), order = ref([]), history = ref([]), ready = ref(false), saving = ref(false);
  const testingId = ref(null), testLogId = ref(null), testLog = ref(''), message = ref('正在读取来源…'), kind = ref('');
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
    let task;
    try { task = JSON.parse(storage?.getItem('ad-killer-by-nfqueue-source-test') || 'null'); } catch { /* invalid saved task */ }
    if (validJob(task?.id)) {
      const row = rows.value.find(s => s.url === task.url);
      if (row) void trackTest(task.id, row);
    }
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
    if (saving.value || testing.value) return;
    const error = sourceUrlError(row.url, rows.value, row.id);
    if (error) { status(error, 'error'); return; }
    editor.value = null; testingId.value = row.id; testLogId.value = row.id; testLog.value = ''; status('正在创建单源测试任务…');
    const result = await api.command('--source-test ' + shellQuote(row.url)), data = lastJson(result.stdout);
    if (!life.active) return;
    if (result.code || data?.status !== 'started' || !validJob(data.job_id)) { testingId.value = null; status('无法启动测试：' + (result.stderr || '后台任务创建失败'), 'error'); return; }
    remember('ad-killer-by-nfqueue-source-test', JSON.stringify({ id: data.job_id, url: row.url }));
    await trackTest(data.job_id, row);
  }
  async function trackTest(id, row) {
    testingId.value = row.id; testLogId.value = row.id; status(`正在测试：${row.url}（不替换当前规则）`);
    while (life.active) {
      const text = await api.readJob(id), result = await api.command('--update-status ' + id), state = jobObservation(result);
      if (!life.active) return;
      testLog.value = text;
      if (!state) { status('暂时无法查询测试状态，正在重试；后台任务不会被停止。', 'warning'); await life.wait(2000); continue; }
      if (state.status === 'running') { await life.wait(1000); continue; }
      const ok = state.status === 'success' && Number(state.exit_code) === 0;
      const unavailable = ['not_found', 'invalid_state', 'invalid_job'].includes(state.status);
      status(ok ? '来源测试通过；未替换规则。有效行数与过滤数量见来源下方日志。' : unavailable ? '来源测试任务记录不可用，请重新测试；当前规则未改变。' : '来源测试失败或中断；当前规则未改变。原因见来源下方日志，可重新测试。', ok ? 'success' : 'error');
      testingId.value = null; remember('ad-killer-by-nfqueue-source-test', null); return;
    }
  }
  const beforeUnload = event => { if (dirty.value) { event.preventDefault(); event.returnValue = ''; } };
  globalThis.window?.addEventListener('beforeunload', beforeUnload);
  onScopeDispose(() => globalThis.window?.removeEventListener('beforeunload', beforeUnload));
  return { rows, displayRows, history, ready, saving, dirty, testing, testingId, testLogId, testLog, message, kind,
    editor, editorError, enabledCount, ordinaryCount, load, toggle, edit, applyEdit, remove, save, test };
}
