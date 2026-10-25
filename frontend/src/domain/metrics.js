const integer = value => Number.isSafeInteger(value) && value >= 0;
const boot = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
export function metricOwner(metrics) {
  return metrics?.owner_verified === true && integer(metrics.owner_pid) && metrics.owner_pid > 0 &&
    integer(metrics.owner_started) && metrics.owner_started > 0 && boot.test(metrics.owner_boot)
    ? `${metrics.owner_boot}:${metrics.owner_pid}:${metrics.owner_started}` : '';
}
export function ruleHits(metrics) {
  if (!integer(metrics?.block_suffix_hits) || !integer(metrics?.block_exact_hits)) return null;
  const total = metrics.block_suffix_hits + metrics.block_exact_hits;
  return Number.isSafeInteger(total) ? total : null;
}
export function metricQuality(state, queryFailed, observedAt, now) {
  const metrics = state?.metrics;
  if (!metrics) return 'missing';
  if (!state?.running) return 'inactive';
  if (!['current', 'stale'].includes(metrics.status))
    return ['missing', 'invalid', 'unreadable', 'clock_invalid', 'inactive'].includes(metrics.status) ? metrics.status : 'unverified';
  if (!metricOwner(metrics) || metrics.owner_pid !== state.daemon_pid) return 'unverified';
  if (ruleHits(metrics) === null || !integer(metrics.updated_at)) return 'invalid';
  if (!integer(metrics.age_ms) || !Number.isFinite(observedAt) || !Number.isFinite(now) || now < observedAt) return 'clock_invalid';
  const age = metrics.age_ms + now - observedAt;
  if (!Number.isFinite(age) || age > Number.MAX_SAFE_INTEGER) return 'clock_invalid';
  if (metrics.status === 'stale' || age > 45000) return 'stale';
  return queryFailed ? 'query_failed' : 'current';
}
