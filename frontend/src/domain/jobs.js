import { jobObservation, lastJson, validJob } from './rules.js';

export async function discoverJobs(api, life, types) {
  if (!life.active) return { cancelled: true };
  try {
    const result = await api.command('--jobs-list'), value = lastJson(result?.stdout);
    if (!life.active) return { cancelled: true };
    if (result?.code || value?.status !== 'ok' || !Array.isArray(value.jobs) || value.jobs.length > 2)
      return { unknown: true };
    const relevant = value.jobs.filter(job => types.includes(job?.type));
    if (value.jobs.some(job => !validJob(job?.job_id) || !['rules', 'source', 'download', 'install'].includes(job?.type)))
      return { unknown: true };
    return { job: relevant[0] || null };
  } catch { return !life.active ? { cancelled: true } : { unknown: true }; }
}

// Keep only a bounded display tail. Bytes are decoded across page boundaries;
// reset epochs/explicit tail seeks also reset the streaming UTF-8 decoder.
export function createJobLogReader(api, id) {
  let cursor = '', decoder = new TextDecoder(), text = '', legacy = '';
  const append = value => { text = (text + value).slice(-65536); };
  return {
    get text() { return text; },
    async read(tail = false) {
      if (!api.readJobChunk) {
        // Compatibility for injected test adapters, not a production shell
        // fallback: createApi always exposes the fixed-target chunk endpoint.
        const next = String(await api.readJob?.(id) || '').slice(-65536);
        const delta = next.startsWith(legacy) ? next.slice(legacy.length) : next;
        legacy = text = next;
        return { delta, more: false, reset: false };
      }
      const result = await api.readJobChunk(id, tail ? 'tail' : cursor), page = lastJson(result?.stdout);
      if (!result?.code && page?.status === 'not_ready') return { delta: '', more: false, pending: true };
      if (result?.code || page?.status !== 'ok' || typeof page.cursor !== 'string' ||
          page.cursor.length > 150 || !/^[A-Za-z0-9-]+:\d+$/.test(page.cursor) ||
          typeof page.data_base64 !== 'string' || page.data_base64.length > 21848 ||
          !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(page.data_base64) ||
          typeof page.reset !== 'boolean' || typeof page.more !== 'boolean' ||
          !Number.isSafeInteger(page.omitted_bytes) || page.omitted_bytes < 0) throw new Error('任务日志片段未能核实');
      const bytes = Uint8Array.from(atob(page.data_base64), byte => byte.charCodeAt(0));
      if (bytes.length > 16384) throw new Error('任务日志片段超过上限');
      if (page.reset) { decoder = new TextDecoder(); text = ''; }
      const delta = decoder.decode(bytes, { stream: true });
      cursor = page.cursor; append(delta);
      return { delta, more: page.more, reset: page.reset, reason: page.reset_reason, omitted: page.omitted_bytes };
    },
    finish() { const delta = decoder.decode(); append(delta); return delta; },
  };
}

// Visibility pauses only observation, never the worker. Five consecutive
// query failures stop with an unknown outcome and preserve resumable identity.
export async function observeJob(api, id, life, options = {}) {
  const { onState = () => {}, onLog = () => {}, onRetry = () => {}, document = globalThis.document } = options;
  const interval = Math.max(0, options.interval ?? 1000), maxErrors = options.maxErrors ?? 5;
  const reader = createJobLogReader(api, id);
  if (!validJob(id)) return { kind: 'unknown', text: '', reason: 'invalid_job' };
  let errors = 0, polls = 0, logError = false;
  while (life.active) {
    if (document?.hidden) { await life.wait(1000); continue; }
    let state;
    try {
      const result = await api.command('--update-status ' + id);
      if (!life.active) return { kind: 'cancelled', text: reader.text };
      state = jobObservation(result);
      if ((state?.job_id && state.job_id !== id) || ['invalid_state', 'invalid_job'].includes(state?.status) ||
          (state?.type && options.types && !options.types.includes(state.type)) ||
          (api.readJobChunk && state && state.job_id !== id) ||
          (['success', 'failed'].includes(state?.status) && (!Number.isInteger(state.exit_code) || state.exit_code < 0 ||
            (state.status === 'success') !== (state.exit_code === 0)))) state = null;
    } catch { state = null; }
    if (!life.active) return { kind: 'cancelled', text: reader.text };
    if (!state) {
      errors++;
      onRetry(errors);
      if (errors >= maxErrors) return { kind: 'unknown', text: reader.text, reason: 'query_unverified' };
      await life.wait(Math.min(8000, interval * 2 ** (errors - 1))); continue;
    }
    errors = 0;
    if (state.status === 'not_found') return { kind: 'unavailable', state, text: reader.text };
    // Do not launch log reads after the WebView has been hidden or disposed.
    if (document?.hidden) continue;
    try {
      const chunk = await reader.read();
      if (!life.active) return { kind: 'cancelled', text: reader.text };
      if (chunk.delta || chunk.reset) onLog(chunk, reader.text);
      if (!life.active) return { kind: 'cancelled', text: reader.text };
      if (state.status !== 'running' && chunk.more && !document?.hidden) {
        const last = await reader.read(true);
        if (!life.active) return { kind: 'cancelled', text: reader.text };
        onLog(last, reader.text);
      }
      logError = !!chunk.pending;
    } catch { logError = true; }
    if (!life.active) return { kind: 'cancelled', text: reader.text };
    onState(state, reader.text, logError);
    if (!life.active) return { kind: 'cancelled', text: reader.text };
    if (state.status !== 'running') {
      const delta = reader.finish(); if (delta) onLog({ delta }, reader.text);
      return { kind: 'terminal', state, text: reader.text, logError };
    }
    polls++;
    await life.wait(Math.min(5000, interval * (1 + Math.floor(polls / 10))));
  }
  return { kind: 'cancelled', text: reader.text };
}
