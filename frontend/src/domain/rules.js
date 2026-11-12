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

export function sourceIssue(reason = '') {
  if (/HTTP (404|410)/i.test(reason)) return '地址不存在或已失效，请编辑来源地址或停用该来源。';
  if (/HTTP (403|401)/i.test(reason)) return '服务器拒绝访问，请稍后重试或更换来源。';
  if (/HTTP 429/i.test(reason)) return '服务器请求过多，稍后同步会再次尝试。';
  if (/HTTP 5\d\d/i.test(reason)) return '来源服务器暂时不可用，请稍后重试。';
  if (/timeout|timed.out|预算|budget/i.test(reason)) return '本次请求超时，可再次同步重试。';
  if (/resolve|network|connect|TLS|SSL|download/i.test(reason)) return '网络连接失败，请检查网络后再次同步。';
  if (/html|markup|网页/i.test(reason)) return '下载到的是网页而非规则，请检查来源地址。';
  if (/empty|no_.*rules|ratio|unsupported/i.test(reason)) return '内容为空或有效规则不足，请测试或更换来源。';
  if (/limit|超限/i.test(reason)) return '规则超过资源上限，请改用较小的规则列表。';
  return '该来源未通过检查，可先测试该来源，或稍后再次同步。';
}

export function sourceHistory(source, entries) {
  if (!source.enabled) return { label: '已停用', detail: '不参与同步；已加载规则在下次完整同步后更新。', kind: '', needsAction: false };
  const entry = entries.find(s => s.url === source.url);
  if (!entry) return { label: '尚未同步', detail: '到概览点击“同步开源规则”。', kind: '', needsAction: false };
  const accepted = entry.status === 'accepted', published = entry.publish_status === 'published';
  const pending = ['queued', 'downloading', 'validating'].includes(entry.status);
  const incomplete = entry.status === 'not_attempted' || /parallel_source_cancelled|budget_cancelled/.test(entry.reason || '');
  const fallback = accepted && entry.provenance === 'cache_fallback';
  const fresh = accepted && entry.provenance === 'cache_fresh';
  const revalidated = accepted && entry.provenance === 'revalidated';
  let label, detail, kind = '', needsAction = false;
  if (pending) {
    label = { queued: '等待处理', downloading: '正在下载', validating: '正在校验' }[entry.status];
    detail = entry.status === 'downloading' && Number(entry.attempts) > 0 ? `第 ${entry.attempts} 次尝试，共最多 ${entry.max_attempts || 1} 次。` : entry.provenance === 'cache_fresh' ? '正在核验缓存内容，无需重新下载。' : '';
  } else if (incomplete) {
    label = '未完成'; kind = 'warning'; needsAction = true;
    detail = '本次同步提前结束，下次同步会继续检查该来源。';
  } else if (!accepted) {
    label = entry.status === 'download_error' || /HTTP \d{3}|download|timeout|resolve|connect/i.test(entry.reason || '') ? '下载失败' : '内容不可用';
    kind = 'error'; needsAction = true; detail = sourceIssue(entry.reason);
    if (Number(entry.attempts) > 1) detail += ` 已尝试 ${entry.attempts} 次。`;
  } else if (!published) {
    label = fallback ? '旧缓存可用' : fresh ? '缓存已就绪' : '已准备好';
    kind = fallback ? 'warning' : ''; needsAction = fallback;
    detail = (fallback ? '本次下载失败，旧缓存已校验可用。' : fresh ? '已核验有效缓存，无需重新下载。' : revalidated ? '服务器已确认缓存内容未变化。' : '下载与校验成功。') + '等待其余来源通过后一起更新规则。';
  } else {
    label = fallback ? '使用旧缓存' : fresh ? '使用缓存' : revalidated ? '无需更新' : '已更新';
    kind = fallback ? 'warning' : 'success'; needsAction = fallback;
    const count = (Number(entry.accepted_hosts) || 0) + (Number(entry.accepted_adguard) || 0);
    detail = `${count.toLocaleString()} 条有效规则（合并去重前）`;
    if (entry.format === 'hagezi-pro-minus-fake') detail += `，已排除 Fake 中的 ${Number(entry.difference_removed || 0).toLocaleString()} 条域名`;
    if (fresh || fallback) {
      const age = Number(entry.cache_age_seconds), verified = Number(entry.cache_verified_at);
      const known = /^\d+$/.test(String(entry.cache_age_seconds ?? '')) && /^\d+$/.test(String(entry.cache_verified_at ?? '')) && Number.isSafeInteger(age) && age >= 0 && Number.isSafeInteger(verified) && verified > 0;
      const ageText = !known ? '年龄未核实' : age < 60 ? '不足 1 分钟' : age < 3600 ? `${Math.ceil(age / 60)} 分钟` : `${(age / 3600).toFixed(1)} 小时`;
      detail += `；缓存距下载核验 ${ageText}。`;
      if (fallback) detail += '本次下载失败，暂用旧缓存，下次同步仍会重试。';
    } else if (revalidated) detail += '；服务器确认内容未变化。';
  }
  const date = new Date(Number(entry.checked_at) * 1000);
  const known = Number(entry.checked_at) > 0 && Number.isFinite(date.getTime());
  return { label, detail, kind, needsAction, pending, technical: entry.reason || entry.warning || '',
    datetime: known ? date.toISOString() : '', time: known ? date.toLocaleString('zh-CN', { hour12: false }) : '' };
}

export function overlap(previous, next) {
  for (let n = Math.min(previous.length, next.length); n > 0; n--) {
    if (previous.slice(-n).every((line, index) => line === next[index])) return n;
  }
  return 0;
}
