import { lastJson } from './rules.js';

export const validApplyRevision = value => typeof value === 'string' && /^[a-f0-9]{64}$/.test(value);
export const confirmedApply = value => value?.status === 'applied' && value.verified === true &&
  value.verification === 'owner_and_commit' && validApplyRevision(value.desired_revision) && value.applied_revision === value.desired_revision;
const brief = value => typeof value === 'string' ? value.slice(0, 500) : '';

export function applyOutcome(phase, error = '', oldVerified = false) {
  const messages = {
    applied: '新配置的生效回执已确认；已有连接仍保留原判定，网络健康以概览为准。',
    deferred: '配置已保存，启动守护后生效。',
    failed: `新配置应用失败。${oldVerified ? '此前的策略仍被保留。' : '请核实概览中的运行状态。'}${brief(error)}`,
    superseded: '该请求已被后续修改替代，请以当前保存的配置为准。',
    pending: '配置已保存并提交，仍在后台应用；请到概览查看结果，勿重复启动。',
    unknown: '配置已保存，但应用结果未能确认；请到概览重新核实，勿重复启动。' + (brief(error) ? ' 原因：' + brief(error) : ''),
    cancelled: '',
  };
  return { phase, message: messages[phase] || messages.unknown,
    level: ['applied', 'deferred'].includes(phase) ? 'ok' : phase === 'failed' ? 'error' : '' };
}

// A durable submission is not an ACK. Observe only this exact intent and stop
// on supersession, disposal, a bounded deadline, or repeated bridge failures.
// Stopping this observer never cancels/starts/restarts the daemon.
export async function observeApply(api, submission, life, options = {}) {
  if (!life.active) return applyOutcome('cancelled');
  const request = lastJson(submission.stdout);
  if (request?.status === 'superseded') return applyOutcome('superseded');
  if (submission.code || !request || !validApplyRevision(request.revision)) return applyOutcome('unknown', request?.message || submission.stderr);
  if (request.status === 'deferred') return applyOutcome('deferred');
  if (request.status !== 'queued') return applyOutcome('unknown');
  const now = options.now || (() => globalThis.performance?.now?.() ?? Date.now());
  const deadline = now() + (options.timeoutMs ?? 9000);
  const attempts = Math.max(1, Math.min(30, options.attempts ?? 24));
  let errors = 0;
  for (let i = 0; i < attempts && life.active && now() < deadline; ++i) {
    await life.wait(options.pollMs ?? 350);
    if (!life.active) return applyOutcome('cancelled');
    let result;
    try { result = await api.command('--apply-status ' + request.revision); }
    catch { result = { code: 1, stdout: '' }; }
    if (!life.active) return applyOutcome('cancelled');
    const state = lastJson(result?.stdout || '');
    if (state?.status === 'superseded') return applyOutcome('superseded');
    if (state?.status === 'unverified') return applyOutcome('unknown');
    if (result?.code !== 0 || !state || state.desired_revision !== request.revision || state.verification !== 'owner_and_commit') {
      if (++errors >= 3) return applyOutcome('unknown');
      continue;
    }
    errors = 0;
    if (state.status === 'deferred') return applyOutcome('deferred');
    if (state.status === 'failed') return applyOutcome('failed', state.error,
      state.verified === true && validApplyRevision(state.applied_revision) && state.applied_revision !== request.revision);
    if (state.status === 'applied') return state.verified === true && state.applied_revision === request.revision
      ? applyOutcome('applied') : applyOutcome('unknown');
    if (!['pending', 'applying'].includes(state.status)) return applyOutcome('unknown');
  }
  return applyOutcome(!life.active ? 'cancelled' : errors ? 'unknown' : 'pending');
}
