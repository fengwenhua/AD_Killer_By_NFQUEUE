export const HAGEZI_ID = 'builtin:hagezi-pro-minus-fake';
export const validJob = id => typeof id === 'string' && id.length <= 96 && /^[0-9]+-[0-9]+$/.test(id);
export const isPackage = value => /^[A-Za-z0-9_]+(\.[A-Za-z0-9_]+)+$/.test(value);
export const lines = text => String(text || '').split(/\r?\n/).map(s => s.trim()).filter(s => s && !s.startsWith('#') && !s.startsWith('!'));
export const keyValues = text => Object.fromEntries(String(text || '').trim().split(/\r?\n/)
  .filter(line => line.includes('=')).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));

export function lastJson(text) {
  for (const line of String(text || '').split(/\r?\n/).reverse()) {
    try { const value = JSON.parse(line.trim()); if (value && typeof value === 'object') return value; } catch { /* CLI log line */ }
  }
  return null;
}

// --update-status deliberately returns exit 1 for a missing/corrupt job record.
// Keep structured lookup errors separate from a bridge outage. A corrupt or
// missing record alone does not prove the worker stopped; observers recover.
export function jobObservation(result) {
  const state = lastJson(result.stdout);
  if (['not_found', 'invalid_state', 'invalid_job'].includes(state?.status)) return state;
  return !result.code && ['running', 'success', 'failed', 'interrupted'].includes(state?.status) ? state : null;
}

export function sourceUrlError(value, sources = [], exceptId = null) {
  if (sources.some(s => s.id !== exceptId && s.url === value)) return '该地址已存在';
  if (value === HAGEZI_ID) return '';
  if (!value || value.length > 4096 || /[\s\x00-\x20\x7f\\]/.test(value)) return '地址为空、过长或包含空白/非法字符';
  try {
    const url = new URL(value);
    if (!['http:', 'https:'].includes(url.protocol) || !url.hostname || url.username || url.password) return '仅支持无账号密码的 HTTP(S) 来源';
  } catch { return '请输入完整的 HTTP(S) 来源地址'; }
  return '';
}

export function normalizeSource(source, id) {
  const builtin = source.url === HAGEZI_ID;
  return { id, url: String(source.url), enabled: !!source.enabled, locked: builtin || !!source.locked,
    name: builtin ? 'HaGeZi Pro − Fake' : String(source.name || ''),
    dependencies: Array.isArray(source.dependencies) ? source.dependencies.map(String) : [] };
}

export const sourceSnapshot = sources => sources.map(({ url, enabled }) => ({ url, enabled }));
export const sourcesDiffer = (sources, snapshot) => JSON.stringify(sourceSnapshot(sources)) !== JSON.stringify(snapshot);
// Only call on initial load or successful save, never on draft toggles/tests.
export const committedSourceOrder = sources => [...sources].sort((a, b) => Number(b.enabled) - Number(a.enabled)).map(s => s.id);

export function parseSourceSummary(text) {
  const parts = String(text || '').split(/\n\s*\[source_\d+\]\s*\n/);
  const head = keyValues(parts[0]);
  if (!/^\d+$/.test(head.source_count || '')) return null;
  const sources = parts.slice(1).map(keyValues).map(s => ({ ...s, checked_at: s.checked_at || head.updated_at || '0', publish_status: head.publish_status || '' }));
  const accepted = Number(head.accepted_sources) || 0, total = Number(head.source_count);
  const filtered = sources.reduce((n, s) => n + (Number(s.rejected_unsupported) || 0) + (Number(s.rejected_too_long) || 0), 0);
  let message = `来源 ${accepted}/${total} 成功；过滤不支持或超长规则 ${filtered.toLocaleString()} 行`;
  if (head.unique_total !== undefined) message += `；去重 ${Number(head.duplicate_lines || 0).toLocaleString()} 行，来源唯一规则 ${Number(head.unique_total).toLocaleString()} 条（含白名单 ${Number(head.unique_white || 0).toLocaleString()}）`;
  if (accepted < total) message += head.publish_status === 'published' ? '。注意：部分来源失败，本次仅合并成功来源。' :
    head.publish_status === 'incomplete_sources' ? '。部分来源未完整通过，本次未发布，保留整份旧规则。' : '。部分来源失败，请核查发布结果。';
  const cached = sources.filter(s => s.status === 'accepted' && s.provenance === 'cache_fallback').length;
  if (cached) message += `；${cached} 个来源使用缓存，请查看来源卡片中的核验时间`;
  return { head, sources, message, partial: accepted < total, cached };
}

export function publishedSummary(report, jobLog, jobId) {
  return report?.head.publish_status === 'published' && report.head.job_id === jobId && report.head.generation &&
    jobLog.includes(`generation ${report.head.generation} 已原子发布`) ? report : null;
}

export function sourceHistory(source, entries) {
  if (!source.enabled) return { label: '已停用，未参与最近一次同步', detail: '', kind: '' };
  const entry = entries.find(s => s.url === source.url);
  if (!entry) return { label: '尚无正式同步记录', detail: '', kind: '' };
  const accepted = entry.status === 'accepted', ok = accepted && entry.publish_status === 'published';
  let detail;
  if (!accepted) detail = '上次同步未接受：' + (entry.reason || entry.status || '统计不完整，请重新同步');
  else if (!ok) detail = entry.publish_status === 'incomplete_sources' ? '来源未完整通过，本次未发布，仍使用整份旧规则。' : '发布结果未确认，请核查当前规则。';
  else if (entry.format === 'hagezi-pro-minus-fake') detail = `Pro 唯一 ${Number(entry.difference_base_unique || 0).toLocaleString()} / Fake 唯一 ${Number(entry.difference_exclude_unique || 0).toLocaleString()} / 差集剔除 ${Number(entry.difference_removed || 0).toLocaleString()} / 保留 ${Number(entry.accepted_hosts || 0).toLocaleString()} 条精确规则`;
  else detail = `上次同步接受 ${((Number(entry.accepted_hosts) || 0) + (Number(entry.accepted_adguard) || 0)).toLocaleString()} 行，过滤 ${((Number(entry.rejected_unsupported) || 0) + (Number(entry.rejected_too_long) || 0)).toLocaleString()} 行（去重前）`;
  const date = new Date(Number(entry.checked_at) * 1000);
  const known = Number(entry.checked_at) > 0 && Number.isFinite(date.getTime());
  const cached = ok && entry.provenance === 'cache_fallback', revalidated = ok && entry.provenance === 'revalidated';
  if (cached) {
    const age = Number(entry.cache_age_seconds), verified = Number(entry.cache_verified_at);
    const ageText = /^\d+$/.test(String(entry.cache_age_seconds ?? '')) && /^\d+$/.test(String(entry.cache_verified_at ?? '')) &&
      Number.isSafeInteger(age) && age >= 0 && Number.isSafeInteger(verified) && verified > 0 ?
      age < 60 ? '不足 1 分钟' : age < 3600 ? `${Math.ceil(age / 60)} 分钟` : `${(age / 3600).toFixed(1)} 小时` : '年龄未核实';
    detail += `；上次同步使用已核验缓存，距核验 ${ageText}。` + (entry.warning ? ` 原因：${entry.warning}` : '');
  } else if (revalidated) detail += '；服务器已确认缓存内容未变化（304）。';
  return { label: cached ? '⚠ 已同步（使用缓存）' : ok ? '✓ 同步成功' : accepted ? '✕ 发布未确认（下载校验已通过）' : entry.status === 'not_attempted' ? '未继续请求（本次同步已中止）' : '✕ 同步失败', detail,
    kind: cached ? 'warning' : ok ? 'success' : 'error', datetime: known ? date.toISOString() : '',
    time: known ? date.toLocaleString('zh-CN', { hour12: false }) : '时间未记录，请重新同步' };
}

export function overlap(previous, next) {
  for (let n = Math.min(previous.length, next.length); n > 0; n--) {
    if (previous.slice(-n).every((line, index) => line === next[index])) return n;
  }
  return 0;
}
