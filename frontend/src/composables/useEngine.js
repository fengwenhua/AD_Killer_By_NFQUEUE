import { ref, computed, watch } from 'vue';
import { PATHS } from '../services/api.js';
import { keyValues, lastJson, overlap } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';
import { confirmedApply, observeApply, validApplyRevision } from '../domain/apply.js';
import { metricOwner, metricQuality, ruleHits } from '../domain/metrics.js';
const cleanupFault = state => ['cleanup_failed', 'cleanup_unknown', 'cleanup_incomplete'].includes(state?.last_error_code);
const applyKey = value => [value?.status, value?.desired_revision, value?.applied_revision, value?.verified, value?.verification].join(':');

export async function waitEngine(target, api, refresh, lifetime) {
  for (let i = 0; i < 40 && lifetime.active; i++) {
    const result = await api.command('--status-fast'), state = lastJson(result.stdout);
    if (result.code || !state) return { ok: false, kind: 'query', message: '无法查询引擎状态，请检查管理器授权与模块版本。' };
    if (target === 'running' && cleanupFault(state)) return { ok: false, kind: 'error', message: state.last_error || '清理结果未核实，请先重试暂停。' };
    if (target === 'running' && state.freezer_state === 'unknown') return { ok: false, kind: 'query', message: '未能核实守护冻结状态，请只读重试并检查权限。' };
    if (state.runtime === target && (target === 'running' ? state.running : !state.running)) {
      const actual = await refresh();
      if (target === 'running' && (actual?.runtime === 'error' || actual?.freezer_state === 'unknown' || cleanupFault(actual))) return { ok: false, kind: 'error', message: actual?.last_error || '引擎健康状态未能核实，请查看运行日志。' };
      if (target === 'running' && actual?.running && ['pending', 'applying'].includes(actual.apply?.status)) { await lifetime.wait(750); continue; }
      if (target === 'running' && !confirmedApply(actual?.apply)) return { ok: false, kind: 'query', message: actual?.apply?.error || '配置生效回执未确认，请重新核实，勿重复启动。' };
      if (target === 'running' && (!actual?.running || !actual.iptables_active)) return { ok: false, kind: 'error', message: actual?.last_error || '引擎状态未能核实，请查看运行日志。' };
      return { ok: true };
    }
    if (state.runtime === 'error') return { ok: false, kind: 'error', message: state.last_error || '引擎启动失败，请查看运行日志。' };
    if (target === 'running' && (state.runtime === 'stopped' || (i >= 4 && !state.running))) return { ok: false, kind: 'exited', message: state.last_error || '启动进程已退出或被取消，请查看运行日志。' };
    await lifetime.wait(750);
  }
  return { ok: false, kind: 'timeout', message: '等待引擎状态超时，请查看日志；不要连续重复启动。' };
}

export function useEngine(api, feedback, clock = () => globalThis.performance?.now?.() ?? Date.now()) {
  const life = useLifetime();
  const state = ref(null), total = ref(null), action = ref(''), setting = ref(''), scope = ref(0), error = ref('');
  const ready = computed(() => !!state.value);
  const healthUnknown = computed(() => !!state.value?.running && state.value?.freezer_state === 'unknown');
  const checking = ref(false);
  const cleanupFailed = computed(() => cleanupFault(state.value));
  const outdated = computed(() => !!state.value?.running && state.value?.daemon_image_current === false);
  const applyIssue = computed(() => !!state.value?.running && !outdated.value &&
    !confirmedApply(state.value?.apply));
  const canRetryApply = computed(() => applyIssue.value && !healthUnknown.value && !cleanupFailed.value && state.value?.runtime !== 'error' &&
    ['saved', 'failed', 'unverified'].includes(state.value?.apply?.status || 'unverified'));
  const running = computed(() => !!state.value?.running && state.value?.runtime !== 'error' && !healthUnknown.value && !cleanupFailed.value && !applyIssue.value && !!state.value?.iptables_active && state.value?.daemon_image_current !== false);
  const routingLost = computed(() => !!state.value?.running && !state.value?.iptables_active);
  const needsRules = computed(() => ready.value && !running.value && state.value.rules_black === 0 && state.value.rules_white === 0);
  const applyDescription = computed(() => {
    const apply = state.value?.apply;
    const previous = apply?.verified && state.value?.iptables_active ? '此前策略仍在运行。' : '当前接管状态尚未核实。';
    if (apply?.status === 'failed') return `新配置应用失败：${String(apply.error || '请查看运行日志').slice(0, 500)} ${previous}可重试应用，不会重新启动守护。`;
    if (apply?.status === 'saved') return `配置已保存但尚未提交应用。${previous}可点击“重新应用配置”。`;
    if (['pending', 'applying'].includes(apply?.status)) return `已提交配置，正在后台应用。${previous}请等待结果，勿重复启动；可在需要时暂停守护。`;
    return '无法核实当前守护的配置回执，请重新核实状态或查看日志；不会自动重启守护。';
  });
  // One precedence table keeps badge/title/description consistent: an error
  // owner can still have a PID, and a retained old policy is not a new ACK.
  const presentation = computed(() => {
    const show = (kind, badge, title, description) => ({ kind, badge, title, description });
    if (!ready.value) return show('neutral', '未连接', '等待连接管理器', '请在 KernelSU / APatch 中打开模块 WebUI。');
    if (cleanupFailed.value) return show('bad', '清理待核实', '停止结果待核实',
      '停止后的清理未能核实。可重试“暂停守护”，或查看日志中的归属与权限诊断；不会抢占其他模块资源。');
    if (healthUnknown.value) return show('warn', '状态未核实', '守护状态待核实',
      '进程仍存在，但未能核实冻结状态。可重新核实或检查权限；不会自动停止或解冻进程。');
    if (state.value.running && state.value.runtime === 'error') return show('bad', '故障待处理', '守护正在故障退出',
      state.value.last_error || '进程仍存在，但已报告故障。请查看运行日志，或重试暂停并核实清理结果。');
    if (applyIssue.value) {
      const phase = state.value.apply?.status;
      return show('warn', phase === 'failed' ? '应用失败' : phase === 'saved' ? '尚未应用' : '生效待确认',
        phase === 'failed' ? '新配置未能生效' : phase === 'saved' ? '配置已保存，尚未应用' : '正在核实配置生效状态', applyDescription.value);
    }
    if (routingLost.value) return show('bad', '路由待恢复', '拦截未正常接管',
      '守护进程仍在，但双栈路由校验未通过。点击“启动守护”安全替换旧实例并重建路由。');
    if (outdated.value) return show('warn', '升级待生效', '旧版本守护仍在运行',
      '安装文件已更新，运行中的守护尚未切换。点击“启动守护”使当前版本生效，不改变开机自启设置。');
    if (running.value) return show('ok', '正在守护', '拦截已开启', '规则已接管当前网络请求，广告域名将被拦截。');
    if (state.value.runtime === 'error') return show('bad', '上次启动失败', '当前未运行，上次启动失败', state.value.last_error || '引擎已退出，请查看运行日志与诊断。');
    if (state.value.runtime === 'starting') return show('warn', '启动中', '正在准备守护', '正在加载规则、建立队列与路由，请稍候。');
    return show('neutral', '已暂停', '当前未接管网络', needsRules.value
      ? '尚无可用规则，请先点击“同步开源规则”，同步成功后再启动守护。' : '本模块未接管网络；其他网络模块的状态请分别检查。');
  });
  const kind = computed(() => presentation.value.kind), badge = computed(() => presentation.value.badge);
  const title = computed(() => presentation.value.title), description = computed(() => error.value || presentation.value.description);
  const metricsStale = ref(false);
  const metricClock = ref(clock()), metricObservedAt = ref(null);
  const tickMetrics = () => { metricClock.value = clock(); };
  watch(() => state.value?.metrics, () => { tickMetrics(); metricObservedAt.value = metricClock.value; }, {flush:'sync'});
  const metricsQuality = computed(() => metricQuality(state.value, metricsStale.value, metricObservedAt.value, metricClock.value));
  // Keep the exported name for callers; this number is domain rule hits only.
  const blockedTotal = computed(() => ['current', 'stale', 'query_failed'].includes(metricsQuality.value) ? ruleHits(state.value?.metrics) : null);
  const metricsNotice = computed(() => ({
    current:'规则命中次数 · 守护重启后重新累计', stale:'统计已过期，保留上次记录', query_failed:'统计暂不可用，保留上次记录',
    missing:'等待统计数据', inactive:'等待本次守护统计', invalid:'统计内容未能核实', unreadable:'无法读取统计数据',
    clock_invalid:'统计时间未能核实', unverified:'统计归属未核实，等待当前守护数据',
  }[metricsQuality.value]));
  let stateRevision=0, metricsRefresh;
  async function refreshMetrics() {
    if (metricsRefresh) return metricsRefresh;
    const revision=stateRevision;
    metricsRefresh=(async()=>{
      const result=await api.command('--status-fast'), next=lastJson(result.stdout);
      if (!life.active || revision!==stateRevision) return;
      if (result.code || !next || typeof next.running!=='boolean') { metricsStale.value=true; return; }
      metricsStale.value=false;
      if (cleanupFault(next)) {
        state.value={...state.value,running:next.running,runtime:next.runtime,last_error:next.last_error,last_error_code:next.last_error_code,metrics:next.metrics};
        return;
      }
      if (next.running && next.freezer_state === 'unknown') {
        state.value={...state.value,running:true,freezer_state:'unknown',frozen:null,freezer_error:next.freezer_error,metrics:next.metrics,
          ...(next.iptables_active===false ? {iptables_active:false} : {}),
          ...(next.daemon_image_current===false ? {daemon_image_current:false} : {})};
        return;
      }
      const oldOwner = metricOwner(state.value?.metrics), nextOwner = metricOwner(next.metrics);
      if (next.running && ((Number.isSafeInteger(next.daemon_pid) && Number.isSafeInteger(state.value?.daemon_pid) && next.daemon_pid !== state.value.daemon_pid) ||
          (oldOwner && nextOwner && oldOwner !== nextOwner))) {
        state.value={...state.value,running:true,daemon_pid:next.daemon_pid,metrics:next.metrics,apply:{status:'unverified',verified:false}};
        if (!await refresh()) metricsStale.value=true;
        return;
      }
      if (applyKey(next.apply) !== applyKey(state.value?.apply)) {
        // A fast response can revoke an ACK, but only full route validation
        // can promote a newly acknowledged revision to healthy.
        state.value={...state.value,apply:confirmedApply(next.apply) ? {...next.apply,status:'unverified',verified:false} : next.apply || {status:'unverified',verified:false},metrics:next.metrics};
        if (!await refresh()) metricsStale.value=true;
        return;
      }
      // Entry-only polling may downgrade health, never make an approximate
      // route green. Recoveries and faults get a full, read-only verification.
      if (next.running && (next.iptables_active===false || next.daemon_image_current===false || !running.value)) {
        state.value={...state.value,metrics:next.metrics,
          ...(next.iptables_active===false ? {iptables_active:false} : {}),
          ...(next.daemon_image_current===false ? {daemon_image_current:false} : {})};
        if (!await refresh()) metricsStale.value=true;
        return;
      }
      if (!next.running) state.value={...state.value,running:false,iptables_active:false,runtime:next.runtime,last_error:next.last_error,last_error_code:next.last_error_code,freezer_state:next.freezer_state,frozen:next.frozen};
      else if (running.value) state.value={...state.value,metrics:next.metrics};
    })();
    try { return await metricsRefresh; } finally {metricsRefresh=null;}
  }
  let refreshing;
  async function refresh() {
    if (refreshing) return refreshing;
    checking.value = true;
    refreshing = (async () => {
      const result = await api.command('--status'), next = lastJson(result.stdout);
      if (result.code || !next || typeof next.running!=='boolean') { if (life.active) metricsStale.value=true; feedback.log(result.stderr || '无法读取引擎状态', 'error'); return null; }
      if (!life.active) return null;
      state.value = next;
      metricsStale.value = false;
      stateRevision++;
      scope.value = Number(next.scope) === 1 ? 1 : 0;
      const known = Number.isSafeInteger(next.rules_black) && next.rules_black >= 0 && Number.isSafeInteger(next.rules_white) && next.rules_white >= 0;
      const actualKnown = next.apply?.verified === true && next.apply.verification === 'owner_and_commit' && validApplyRevision(next.apply.applied_revision);
      if (known && (!next.running || actualKnown)) total.value = Number.isSafeInteger(next.rules_black + next.rules_white) ? next.rules_black + next.rules_white : null;
      else if (next.running || next.apply) total.value = null; // never override ACK uncertainty with a stale statistics file
      else {
        const stats = keyValues(await api.readFile(PATHS.stats));
        if (life.active) total.value = stats.black !== undefined ? (Number(stats.black) || 0) + (Number(stats.white) || 0) : null;
      }
      return next;
    })();
    try { return await refreshing; } finally { refreshing = null; checking.value = false; }
  }
  async function control(verb) {
    if (action.value || setting.value || !ready.value) return;
    if (verb === 'start' && healthUnknown.value) { feedback.toast('请先重新核实守护状态'); return; }
    if (verb === 'start' && cleanupFailed.value) { feedback.toast('请先重试暂停并核实清理结果'); return; }
    if (verb === 'start' && applyIssue.value) { feedback.toast('请先核实或重新应用配置，勿重复启动守护'); return; }
    action.value = verb; error.value = '';
    const label = verb === 'start' ? '启动' : '停止';
    feedback.log(`${label}底层拦截引擎…`);
    await life.wait(32); // let the WebView paint before a synchronous native call
    try {
      const result = await api.command(`--${verb}`), reply = lastJson(result.stdout);
      if (reply?.status === 'cancelled' || reply?.status === 'error_start_cancelled') {
        const message = '本次启停请求已取消，请以刷新后的实际状态为准。';
        feedback.log(message); feedback.toast(message); return;
      }
      if (verb === 'start' && reply?.status === 'error_rules_empty') {
        const message = '请先点击“同步开源规则”，同步成功后再启动守护。';
        error.value = message; feedback.log(message); feedback.toast(message);
        return;
      }
      let outcome;
      if (result.code || !reply || String(reply.status || '').startsWith('error') || reply.status === 'fork_failed') {
        const query = lastJson((await api.command('--status-fast')).stdout);
        outcome = { ok: false, kind: 'error', message: reply?.message || query?.last_error || result.stderr.trim() || '命令执行失败，请查看运行日志。' };
      } else outcome = await waitEngine(verb === 'start' ? 'running' : 'stopped', api, refresh, life);
      if (!life.active) return;
      const message = outcome.ok ? `拦截引擎与规则已${verb === 'start' ? '启动' : '停止'}` : `${label}${outcome.kind === 'timeout' ? '等待超时：' : '失败：'}${outcome.message}`;
      feedback.log(message, outcome.ok ? 'ok' : 'error');
      if (!outcome.ok) { error.value = message; feedback.consoleOpen.value = true; }
      feedback.toast(outcome.ok ? message : `${label}${outcome.kind === 'timeout' ? '超时' : '失败'}`);
    } finally { action.value = ''; if (life.active) await refresh(); }
  }
  async function verifyHealth() {
    if (checking.value || action.value) return;
    checking.value = true;
    try {
      await life.wait(32);
      if (life.active) return await refresh();
    } finally { checking.value = false; }
  }
  async function changeSetting(name, value) {
    if (!ready.value || action.value || setting.value) return;
    setting.value = name; error.value = '';
    const arg = ['mode', 'scope'].includes(name) ? value : value ? 'on' : 'off';
    try {
      await life.wait(32);
      if (!life.active) return;
      const result = await api.command(`--${name} ${arg}`), reply = lastJson(result.stdout);
      if (!life.active) return;
      if (result.code || (name === 'autostart' && reply?.autostart !== value)) {
        feedback.log(`${name === 'autostart' ? '开机自启' : '配置'}设置失败：${result.stderr || '请刷新后重试'}`, 'error');
        feedback.toast('设置失败');
      } else {
        if (['mode', 'scope', 'quic', 'dev'].includes(name)) {
          const outcome = await observeApply(api, result, life);
          if (!life.active) return;
          feedback.log(outcome.message, outcome.level); feedback.toast(outcome.message);
        } else {
          feedback.log(name === 'autostart' ? value ? '已开启开机自启；手动暂停仍会保留' : '已关闭开机自启，当前运行状态不变' : '配置已保存', 'ok');
          feedback.toast('设置已保存');
        }
      }
    } catch (cause) {
      if (life.active) { feedback.log('设置结果未能确认：' + cause.message, 'error'); feedback.toast('结果未确认，请重新核实'); }
    } finally { setting.value = ''; if (life.active) await refresh(); }
  }
  async function retryApply() {
    if (!canRetryApply.value || action.value || setting.value) return;
    setting.value = 'apply'; error.value = '';
    try {
      await life.wait(32);
      if (!life.active) return;
      const outcome = await observeApply(api, await api.command('--apps-apply'), life);
      if (life.active) { feedback.log(outcome.message, outcome.level); feedback.toast(outcome.message); }
    } catch (cause) {
      if (life.active) { feedback.log('应用结果未能确认：' + cause.message, 'error'); feedback.toast('结果未确认，请重新核实'); }
    } finally { setting.value = ''; if (life.active) await refresh(); }
  }
  watch(() => running.value && !!state.value?.dev_mode && !!feedback.consoleOpen?.value, async (enabled, _, onCleanup) => {
    if (!enabled) return;
    let cancelled = false, previous = [];
    onCleanup(() => { cancelled = true; });
    while (life.active && !cancelled) {
      const result = await api.readDns();
      if (cancelled || !life.active) break;
      if (!result.code) {
        const next = result.stdout.split(/\r?\n/).map(s => s.trim()).filter(s => s && s.length < 400);
        next.slice(overlap(previous, next)).forEach(line => feedback.log(line)); previous = next;
      }
      await life.wait(1000);
    }
  });
  return { state, total, scope, action, setting, ready, running, healthUnknown, cleanupFailed, applyIssue, canRetryApply, checking, kind, badge, title, description, blockedTotal, metricsStale, metricsQuality, metricsNotice, tickMetrics, refreshMetrics, refresh, verifyHealth, retryApply, control, changeSetting };
}
