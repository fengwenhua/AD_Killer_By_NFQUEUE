import { HAGEZI_ID, parseSourceSummary, sourceHistory, sourceUrlError, validJob } from './rules.js';

const states = new Set(['queued', 'downloading', 'validating', 'accepted', 'download_error', 'content_rejected', 'not_attempted']);
export function sourceName(url) {
  if (url === HAGEZI_ID) return 'HaGeZi Pro − Fake';
  try { const value = new URL(url), file = value.pathname.split('/').filter(Boolean).pop(); return value.hostname + (file ? ' · ' + file.slice(0, 50) : ''); }
  catch { return '规则来源'; }
}
export function syncProgress(text, id, jobStatus = 'running') {
  if (typeof text !== 'string' || text.length > 512 * 1024 || !validJob(id)) return null;
  const report = parseSourceSummary(text);
  if (!report || report.head.job_id !== id || !/^\d+$/.test(report.head.source_count) ||
      report.sources.length < 1 || report.sources.length > 65 || Number(report.head.source_count) !== report.sources.length ||
      new Set(report.sources.map(s => s.url)).size !== report.sources.length ||
      report.sources.some(s => sourceUrlError(s.url) || !states.has(s.status))) return null;
  const sources = report.sources.map(s => (jobStatus !== 'running' && ['queued', 'downloading', 'validating'].includes(s.status)) || /parallel_source_cancelled/.test(s.reason || '')
    ? { ...s, status: 'not_attempted' } : s);
  const rows = sources.map(source => ({ ...source, name: sourceName(source.url),
    display: sourceHistory({ url: source.url, enabled: true }, sources) }));
  const count = predicate => rows.filter(predicate).length;
  const ready = count(s => s.status === 'accepted');
  const failed = count(s => ['download_error', 'content_rejected'].includes(s.status));
  const pending = count(s => s.display.pending);
  const needsRetry = count(s => s.display.needsAction);
  const fallback = count(s => s.status === 'accepted' && s.provenance === 'cache_fallback');
  const cached = count(s => s.status === 'accepted' && ['cache_fresh', 'revalidated', 'cache_fallback'].includes(s.provenance));
  const done = ready + failed;
  const phase = report.head.publish_status;
  const title = jobStatus === 'interrupted' ? '同步已中断，请核实规则状态' :
    phase === 'published' ? jobStatus === 'running' ? '规则已保存，正在确认结果' : jobStatus !== 'success' ? '已有保存记录，请核实任务结果' : fallback ? '规则已保存，部分来源暂用旧缓存' : failed ? '部分来源失败，请核实保存结果' : '同步完成，规则已保存' :
    phase === 'publishing' ? '来源已处理，正在整理规则' :
    phase === 'incomplete_sources' ? '规则未更新，继续使用原规则' :
    jobStatus === 'running' ? `正在处理来源 ${done} / ${rows.length}` : '本次同步未完成';
  return { ...report, sources, id, title, total: rows.length, done, ready, failed, pending, cached, needsRetry,
    rows, phase, jobStatus, kind: phase === 'published' && jobStatus === 'success' && !fallback && !failed ? 'success' : jobStatus === 'running' ? '' : 'warning' };
}
