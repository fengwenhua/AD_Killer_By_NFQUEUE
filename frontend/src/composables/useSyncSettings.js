import { ref } from 'vue';
import { useLifetime } from './useLifetime.js';
import { afterPaint } from '../services/paint.js';

export const SYNC_DEFAULT = 10, SYNC_MAX = 20;
export const SYNC_INTERVAL_DEFAULT = 30, SYNC_INTERVAL_MAX = 1440;
export const SYNC_RETRIES_DEFAULT = 2, SYNC_RETRIES_MAX = 5;
function boundedNumber(value, max) {
  const text = String(value ?? '');
  const count = /^\d+$/.test(text) ? Number(text) : NaN;
  return Number.isInteger(count) && count >= 0 && count <= max ? count : null;
}
export const intervalNumber = value => boundedNumber(value, SYNC_INTERVAL_MAX);
export const retriesNumber = value => boundedNumber(value, SYNC_RETRIES_MAX);
export function concurrencyNumber(value) {
  const text = String(value ?? '');
  const count = /^\d+$/.test(text) ? Number(text) : NaN;
  return Number.isInteger(count) && count >= 1 && count <= SYNC_MAX ? count : null;
}
export function useSyncSettings(api, paint = afterPaint) {
  const life = useLifetime();
  const count = ref(SYNC_DEFAULT), draft = ref(String(SYNC_DEFAULT));
  const interval = ref(SYNC_INTERVAL_DEFAULT), intervalDraft = ref(String(SYNC_INTERVAL_DEFAULT));
  const retries = ref(SYNC_RETRIES_DEFAULT), retriesDraft = ref(String(SYNC_RETRIES_DEFAULT));
  const ready = ref(false), busy = ref(false), error = ref(''), message = ref('');
  function reply(result) {
    let value;
    try { value = JSON.parse(result.stdout); } catch { /* failed or old backend */ }
    if (!result || result.code || value?.status !== 'ok' || !Number.isInteger(value.concurrency) || concurrencyNumber(value.concurrency) === null ||
        !Number.isInteger(value.interval_minutes) || intervalNumber(value.interval_minutes) === null ||
        !Number.isInteger(value.retries) || retriesNumber(value.retries) === null)
      throw new Error(value?.message || '无法核实同步设置，请重新读取或升级完整模块。');
    return value;
  }
  async function load() {
    if (!life.active || busy.value) return;
    busy.value = true; error.value = '';
    try {
      const value = reply(await api.command('--sync-settings'));
      if (!life.active) return;
      count.value = value.concurrency; draft.value = String(value.concurrency);
      interval.value = value.interval_minutes; intervalDraft.value = String(value.interval_minutes);
      retries.value = value.retries; retriesDraft.value = String(value.retries); ready.value = true;
    } catch (e) { if (life.active) { ready.value = false; error.value = e.message; } }
    finally { if (life.active) busy.value = false; }
  }
  async function save() {
    if (!life.active || busy.value || !ready.value) return;
    const value = concurrencyNumber(draft.value), minutes = intervalNumber(intervalDraft.value), retryCount = retriesNumber(retriesDraft.value);
    error.value = ''; message.value = '';
    if (value === null) { error.value = `请输入 1–${SYNC_MAX} 之间的整数`; return; }
    if (minutes === null) { error.value = `同步时间间隔须为 0–${SYNC_INTERVAL_MAX} 分钟的整数`; return; }
    if (retryCount === null) { error.value = `重试次数须为 0–${SYNC_RETRIES_MAX} 的整数`; return; }
    busy.value = true;
    try {
      await paint();
      if (!life.active) return;
      const saved = reply(await api.command(`--sync-options ${value} ${minutes} ${retryCount}`));
      if (!life.active) return;
      if (saved.concurrency !== value || saved.interval_minutes !== minutes || saved.retries !== retryCount) throw new Error('保存结果与请求不一致，请重新读取核实。');
      count.value = saved.concurrency; draft.value = String(saved.concurrency);
      interval.value = saved.interval_minutes; intervalDraft.value = String(saved.interval_minutes);
      retries.value = saved.retries; retriesDraft.value = String(saved.retries); message.value = '已保存，下次同步时生效。';
    } catch (e) { if (life.active) error.value = e.message; }
    finally { if (life.active) busy.value = false; }
  }
  return { count, draft, interval, intervalDraft, retries, retriesDraft, ready, busy, error, message, load, save };
}
