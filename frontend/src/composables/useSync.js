import { ref } from 'vue';
import { PATHS } from '../services/api.js';
import { getStorage } from '../services/storage.js';
import { jobObservation, lastJson, parseSourceSummary, publishedSummary, validJob } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';

export function useSync(api, feedback, history, refresh, storage = getStorage()) {
  const life = useLifetime();
  const busy = ref(false), message = ref('同步完成后保存到本地，并更新当前规则。'), kind = ref('');
  function status(text, statusKind = '') { message.value = text; kind.value = statusKind; }
  function remember(id) { try { id ? storage?.setItem('ad-killer-by-nfqueue-update-job', id) : storage?.removeItem('ad-killer-by-nfqueue-update-job'); } catch { /* unavailable storage */ } }
  async function start() {
    if (busy.value) return;
    busy.value = true; status('正在创建后台更新任务…');
    feedback.log('开始同步规则（可在“规则 → 开源规则来源”管理来源）…');
    const result = await api.command('--update-start'), reply = lastJson(result.stdout);
    if (!life.active) return;
    if (result.code || reply?.status !== 'started' || !validJob(reply.job_id)) {
      busy.value = false; status('无法创建同步任务，请查看诊断日志。', 'error');
      feedback.consoleOpen.value = true; feedback.log(result.stderr || '无法创建规则更新任务', 'error'); feedback.toast('同步启动失败'); return;
    }
    remember(reply.job_id); await track(reply.job_id);
  }
  async function track(id) {
    busy.value = true; status('后台正在下载与校验规则…');
    let previous = '';
    while (life.active) {
      const text = await api.readJob(id), result = await api.command('--update-status ' + id), state = jobObservation(result);
      if (!life.active) return;
      if (text && text !== previous) {
        feedback.log(text.startsWith(previous) ? text.slice(previous.length) : text); previous = text;
        status(text.trim().split(/\r?\n/).pop().replace(/^\[[^\]]*\]\s*\[[^\]]*\]\s*/, '') || '正在处理规则…');
      }
      if (!state) { status('暂时无法查询同步状态，正在重试；请勿重复启动同步。', 'warning'); await life.wait(2000); continue; }
      if (state.status === 'running') { await life.wait(1000); continue; }
      const ok = state.status === 'success' && Number(state.exit_code) === 0;
      const report = parseSourceSummary(await api.readFile(PATHS.sources));
      if (!life.active) return;
      if (report?.head.job_id === id) history.value = report.sources;
      const summary = ok ? publishedSummary(report, text, id) : null;
      const partial = ok && (summary ? summary.partial : /下载失败:|来源内容拒绝:|来源累计资源上限|注意：部分来源失败/.test(text));
      const failure = text.trim().split(/\r?\n/).reverse().find(line => /失败|拒绝|错误|无法|不可用|超限/.test(line));
      const missing = ['not_found', 'invalid_state', 'invalid_job'].includes(state.status);
      const detail = missing ? '后台任务记录不存在或不可用，发布结果未确认，请核查当前规则。' : failure ? failure.replace(/^\[[^\]]*\]\s*\[[^\]]*\]\s*/, '') : '请展开诊断日志查看详情';
      const final = ok ? summary?.message || (partial ? '部分来源失败，成功来源已合并保存；请查看诊断日志。' : '规则已校验并合并，成功保存到本地。') : state.status === 'interrupted' ? '更新进程意外中断，发布结果未确认；请核查当前规则。' : '同步失败：' + detail;
      const finalKind = ok ? partial ? 'warning' : 'success' : 'error';
      feedback.log(final, finalKind === 'success' ? 'ok' : finalKind === 'error' ? 'error' : '');
      status(final, finalKind); if (!ok) feedback.consoleOpen.value = true;
      feedback.toast(ok ? partial ? '同步完成，但部分来源失败' : '规则已同步完成' : '同步失败');
      remember(null); busy.value = false; await refresh(); return;
    }
  }
  async function resume() {
    let id; try { id = storage?.getItem('ad-killer-by-nfqueue-update-job'); } catch { /* no storage */ }
    if (validJob(id) && !busy.value) { void track(id); return; }
    const report = parseSourceSummary(await api.readFile(PATHS.sources));
    if (report?.head.publish_status === 'published' && !busy.value && life.active) status('上次同步：' + report.message, report.partial ? 'warning' : 'success');
  }
  return { busy, message, kind, start, resume };
}
