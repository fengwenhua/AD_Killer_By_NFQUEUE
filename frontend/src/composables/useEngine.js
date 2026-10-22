import { ref, computed, watch } from 'vue';
import { PATHS } from '../services/api.js';
import { keyValues, lastJson, overlap } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';

export async function waitEngine(target, api, refresh, lifetime) {
  for (let i = 0; i < 40 && lifetime.active; i++) {
    const result = await api.command('--status-fast'), state = lastJson(result.stdout);
    if (result.code || !state) return { ok: false, kind: 'query', message: '无法查询引擎状态，请检查管理器授权与模块版本。' };
    if (state.runtime === target && (target === 'running' ? state.running : !state.running)) {
      const actual = await refresh();
      if (target === 'running' && (!actual?.running || !actual.iptables_active)) return { ok: false, kind: 'error', message: actual?.last_error || '引擎未正常接管路由，请查看运行日志。' };
      return { ok: true };
    }
    if (state.runtime === 'error') return { ok: false, kind: 'error', message: state.last_error || '引擎启动失败，请查看运行日志。' };
    if (target === 'running' && (state.runtime === 'stopped' || (i >= 4 && !state.running))) return { ok: false, kind: 'exited', message: state.last_error || '启动进程已退出或被取消，请查看运行日志。' };
    await lifetime.wait(750);
  }
  return { ok: false, kind: 'timeout', message: '等待引擎状态超时，请查看日志；不要连续重复启动。' };
}

export function useEngine(api, feedback) {
  const life = useLifetime();
  const state = ref(null), total = ref(null), action = ref(''), setting = ref(''), scope = ref(0), error = ref('');
  const ready = computed(() => !!state.value);
  const running = computed(() => !!state.value?.running && !!state.value?.iptables_active);
  const needsRules = computed(() => ready.value && !running.value && state.value.rules_black === 0 && state.value.rules_white === 0);
  const kind = computed(() => running.value ? 'ok' : state.value?.runtime === 'error' ? 'bad' : state.value?.runtime === 'starting' ? 'warn' : 'neutral');
  const badge = computed(() => !ready.value ? '未连接' : ({ ok: '正在守护', bad: '上次启动失败', warn: '启动中', neutral: '已暂停' })[kind.value]);
  const title = computed(() => !ready.value ? '等待连接管理器' : ({ ok: '拦截已开启', bad: '当前未运行，上次启动失败', warn: '正在准备守护', neutral: '当前未接管网络' })[kind.value]);
  const description = computed(() => error.value || (!ready.value ? '请在 KernelSU / APatch 中打开模块 WebUI。' : running.value ? '规则已接管当前网络请求，广告域名将被拦截。' : kind.value === 'bad' ? state.value.last_error || '引擎已退出，请查看运行日志与诊断。' : kind.value === 'warn' ? '正在加载规则、建立队列与路由，请稍候。' : needsRules.value ? '尚无可用规则，请先点击“同步开源规则”，同步成功后再启动守护。' : '本模块未接管网络；其他网络模块的状态请分别检查。'));
  const blockedTotal = computed(() => {
    const metrics=state.value?.metrics;
    if (!metrics || !Number.isSafeInteger(metrics.updated_at) || metrics.updated_at<=0) return null;
    const values=[metrics.block_suffix_hits,metrics.block_exact_hits];
    if (!values.every(value=>Number.isSafeInteger(value) && value>=0)) return null;
    const total=values[0]+values[1];
    return Number.isSafeInteger(total) ? total : null;
  });
  const metricsStale = ref(false);
  let stateRevision=0, metricsRefresh;
  async function refreshMetrics() {
    if (metricsRefresh) return metricsRefresh;
    const revision=stateRevision;
    metricsRefresh=(async()=>{
      const result=await api.command('--status-fast'), next=lastJson(result.stdout);
      if (!life.active || revision!==stateRevision) return;
      if (result.code || !next || typeof next.running!=='boolean') { metricsStale.value=true; return; }
      metricsStale.value=false;
      if (!next.running) state.value={...state.value,running:false,iptables_active:false,runtime:next.runtime,last_error:next.last_error};
      else if (running.value) state.value={...state.value,metrics:next.metrics};
    })();
    try { return await metricsRefresh; } finally {metricsRefresh=null;}
  }
  let refreshing;
  async function refresh() {
    if (refreshing) return refreshing;
    refreshing = (async () => {
      const result = await api.command('--status'), next = lastJson(result.stdout);
      if (!next || !('running' in next)) { feedback.log(result.stderr || '无法读取引擎状态', 'error'); return null; }
      if (!life.active) return null;
      state.value = next;
      stateRevision++;
      scope.value = Number(next.scope) === 1 ? 1 : 0;
      const stats = keyValues(await api.readFile(PATHS.stats));
      if (life.active) total.value = stats.black !== undefined ? (Number(stats.black) || 0) + (Number(stats.white) || 0)
        : Number.isSafeInteger(next.rules_black) && Number.isSafeInteger(next.rules_white) ? next.rules_black + next.rules_white : null;
      return next;
    })();
    try { return await refreshing; } finally { refreshing = null; }
  }
  async function control(verb) {
    if (action.value || setting.value || !ready.value) return;
    action.value = verb; error.value = '';
    const label = verb === 'start' ? '启动' : '停止';
    feedback.log(`${label}底层拦截引擎…`);
    await life.wait(32); // let the WebView paint before a synchronous native call
    try {
      const result = await api.command(`--${verb}`), reply = lastJson(result.stdout);
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
  async function changeSetting(name, value) {
    if (!ready.value || action.value || setting.value) return;
    setting.value = name;
    const arg = ['mode', 'scope'].includes(name) ? value : value ? 'on' : 'off';
    try {
      const result = await api.command(`--${name} ${arg}`), reply = lastJson(result.stdout);
      if (result.code || (name === 'autostart' && reply?.autostart !== value)) {
        feedback.log(`${name === 'autostart' ? '开机自启' : '配置'}设置失败：${result.stderr || '请刷新后重试'}`, 'error');
        feedback.toast('设置失败');
      } else {
        if (name === 'scope') {
          const applied = await api.command('--apps-apply');
          feedback.log(applied.code ? '范围配置已保存，但应用名单刷新失败' : applied.stdout.includes('deferred') ? '拦截范围与对应名单已保存，启动后生效' : '拦截范围与对应名单已提交，等待后台热重载；失败原因请查看引擎日志', applied.code ? 'error' : 'ok');
        } else feedback.log(name === 'autostart' ? value ? '已开启开机自启；手动暂停仍会保留' : '已关闭开机自启，当前运行状态不变' : '配置已保存', 'ok');
        feedback.toast('设置已保存');
      }
      await refresh();
    } finally { setting.value = ''; }
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
  return { state, total, scope, action, setting, ready, running, kind, badge, title, description, blockedTotal, metricsStale, refreshMetrics, refresh, control, changeSetting };
}
