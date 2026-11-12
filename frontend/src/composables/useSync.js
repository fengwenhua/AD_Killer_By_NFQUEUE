import { ref } from 'vue';
import { PATHS } from '../services/api.js';
import { getStorage } from '../services/storage.js';
import { lastJson, parseSourceSummary, publishedSummary, validJob } from '../domain/rules.js';
import { discoverJobs, observeJob } from '../domain/jobs.js';
import { useLifetime } from './useLifetime.js';
import { syncProgress as parseProgress } from '../domain/syncProgress.js';

export function useSync(api, feedback, history, refresh, storage = getStorage(), pollMs = 1000) {
  const life = useLifetime();
  const progress = ref(null);
  const busy = ref(false), uncertain = ref(false), pendingId = ref('');
  const message = ref('同步完成后保存到本地，并更新当前规则。'), kind = ref('');
  let tracking, pendingNotificationId = '';
  function status(text, statusKind = '') { message.value = text; kind.value = statusKind; }
  function remember(id) { try { id ? storage?.setItem('ad-killer-by-nfqueue-update-job', id) : storage?.removeItem('ad-killer-by-nfqueue-update-job'); } catch { /* unavailable storage */ } }
  function unknown(text) { uncertain.value = true; status(text, 'warning'); }
  async function discover() {
    return discoverJobs(api, life, ['rules', 'source']);
  }
  async function start() {
    if (busy.value) return;
    if (uncertain.value) return resume(); // Read-only retry, never blind resubmission.
    busy.value = true; progress.value = null; status('正在核实后台任务…');
    try {
      const found = await discover();
      if (!life.active || found.cancelled) return;
      if (found.unknown) { unknown('无法核实后台任务。点击“继续查询”，不会重复创建同步。'); return; }
      if (found.job && ['running', 'unverified', 'invalid_state', 'not_found'].includes(found.job.status))
        return await track(found.job.job_id, found.job.type);
      status('正在创建后台更新任务…');
      feedback.log('开始同步规则（可在“规则 → 开源规则来源”管理来源）…');
      const result = await api.command('--update-start'), reply = lastJson(result.stdout);
      if (!life.active) return;
      if (result.code || reply?.status !== 'started' || !validJob(reply.job_id)) {
        unknown(reply?.status === 'busy' ? '已有后台任务尚未结束，点击“继续查询”核实。' : '任务派发结果未确认。点击“继续查询”，不会盲目重开。');
        feedback.log(result.stderr || reply?.message || message.value, 'error'); return;
      }
      remember(reply.job_id); await track(reply.job_id);
    } catch { if (life.active) unknown('任务结果未确认，点击“继续查询”从后台恢复。'); }
    finally { if (life.active) busy.value = false; }
  }
  async function track(id, type = 'rules', { notifyCompletion = true } = {}) {
    if (tracking) return tracking;
    // Recovered terminal jobs are history; keep live/manual intent across an uncertain result.
    let notify = notifyCompletion || pendingNotificationId === id;
    if (type === 'rules' && notify) pendingNotificationId = id;
    pendingId.value = id; uncertain.value = false; busy.value = true;
    if (type === 'rules') remember(id);
    status(type === 'source' ? '发现后台来源测试，正在核实其结束状态；尚未同步规则。' : notify ? '后台正在下载与校验规则…' : '正在读取上次同步结果…');
    tracking = (async () => {
      const outcome = await observeJob(api, id, life, {
        types: [type], interval: pollMs,
        onLog(chunk) {
          if (chunk.reset && ['rotated', 'tail', 'truncated'].includes(chunk.reason)) feedback.log('日志已轮转或已跳至末尾，较早内容未完整显示。');
          if (chunk.delta) feedback.log(chunk.delta);
        },
        onState(state, text, logError) {
          if (type === 'rules') {
            const value = parseProgress(state.progress, id, state.status);
            if (value) { progress.value = value; history.value = value.sources; }
          }
          if (state.status === 'running') {
            notify = true;
            if (type === 'rules') pendingNotificationId = id;
            status((type === 'source' ? '来源测试仍在后台运行' : progress.value?.title || text.trim().split(/\r?\n/).pop()?.replace(/^\[[^\]]*\]\s*\[[^\]]*\]\s*/, '') || '后台正在处理规则') + (logError ? '；日志片段暂不可用，任务状态已读取。' : ''));
          }
        },
        onRetry: () => status('暂时无法核实任务状态，正在退避重试；不会重复同步。', 'warning'),
      });
      if (!life.active || outcome.kind === 'cancelled') return;
      if (outcome.kind === 'unknown') { unknown('任务结果仍未确认，已暂停自动查询。点击“继续查询”；任务可能仍在后台运行。'); return; }
      if (outcome.kind === 'unavailable') {
        remember(null); pendingId.value = ''; unknown('后台任务记录不存在，发布结果未确认。点击“继续查询”核实后台索引。'); return;
      }
      const state = outcome.state, text = outcome.text;
      remember(null); pendingId.value = ''; uncertain.value = false;
      if (type === 'source') {
        status('来源测试已结束，该次测试未执行同步；如需更新规则请点击“同步开源规则”。', state.status === 'success' ? '' : 'warning');
        await refresh(); return;
      }
      const ok = state.status === 'success' && state.exit_code === 0;
      const sourceText = await api.readFile(PATHS.sources);
      const report = parseSourceSummary(sourceText);
      if (!life.active) return;
      if (report?.head.job_id === id) {
        history.value = report.sources;
        progress.value = parseProgress(sourceText, id, state.status) || progress.value;
      }
      const summary = ok ? publishedSummary(report, text, id) : null;
      const partial = ok && (summary ? summary.partial : /下载失败:|来源内容拒绝:|来源累计资源上限|注意：部分来源失败/.test(text));
      const cached = ok && (summary ? summary.cached > 0 : /整组复用.*缓存/.test(text));
      const notUpdated = progress.value?.phase === 'incomplete_sources';
      const final = ok ? summary?.message || (partial ? '部分来源失败，成功来源已合并保存；请查看来源结果。' : cached ? '规则已同步，部分来源使用缓存；下次会重试下载。' : '规则已保存；可在概览核实当前生效状态。') :
        state.status === 'interrupted' ? '同步进程已中断，规则更新结果未确认；请核查概览状态。' :
        notUpdated ? `${progress.value.failed} 个来源未通过，${progress.value.ready} 个已准备好。本次规则未更新，仍使用原规则；可处理失败来源后再次同步。` :
        '同步未完成，请查看来源结果或诊断日志。';
      const finalKind = ok ? partial || cached ? 'warning' : 'success' : 'error';
      const resultMessage = (notify ? '' : '上次同步：') + final;
      feedback.log(resultMessage, finalKind === 'success' ? 'ok' : finalKind === 'error' ? 'error' : '');
      status(resultMessage + (outcome.logError ? ' 日志片段未能完整读取。' : ''), finalKind);
      if (notify) {
        if (!ok && !progress.value) feedback.consoleOpen.value = true;
        feedback.toast(ok ? partial ? '同步完成，但部分来源失败' : cached ? '规则已同步，部分来源使用缓存' : '规则已同步完成' : '同步失败');
      }
      if (pendingNotificationId === id) pendingNotificationId = '';
      await refresh();
    })();
    try { await tracking; }
    catch { if (life.active) unknown('同步结果未能完整核实，请继续查询后台任务。'); }
    finally { tracking = null; if (life.active) busy.value = false; }
  }
  async function resume() {
    if (busy.value) return;
    busy.value = true;
    try {
      let id = pendingId.value;
      try { id ||= storage?.getItem('ad-killer-by-nfqueue-update-job'); } catch { /* no storage */ }
      const found = await discover();
      if (!life.active || found.cancelled) return;
      if (found.job) { void track(found.job.job_id, found.job.type, { notifyCompletion: found.job.status === 'running' }); return; }
      if (validJob(id)) { void track(id, 'rules', { notifyCompletion: false }); return; }
      if (found.unknown) { unknown('后台任务索引未能核实。点击“继续查询”，不会创建新同步。'); return; }
      uncertain.value = false; pendingId.value = ''; remember(null);
      const report = parseSourceSummary(await api.readFile(PATHS.sources));
      if (report?.head.publish_status === 'published' && life.active) status('上次同步：' + report.message, report.partial ? 'warning' : 'success');
      else if (life.active) status('未发现活动同步任务，可点击“同步开源规则”。');
    } catch { if (life.active) unknown('后台任务恢复结果未确认，请继续查询。'); }
    finally { if (life.active && !tracking) busy.value = false; }
  }
  return { progress, busy, uncertain, pendingId, message, kind, start, resume };
}
