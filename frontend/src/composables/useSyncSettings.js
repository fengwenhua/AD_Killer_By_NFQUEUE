import { ref } from 'vue';
import { useLifetime } from './useLifetime.js';
import { afterPaint } from '../services/paint.js';

export const SYNC_DEFAULT = 10, SYNC_MAX = 20;
export function concurrencyNumber(value) {
  const text = String(value ?? '');
  const count = /^\d+$/.test(text) ? Number(text) : NaN;
  return Number.isInteger(count) && count >= 1 && count <= SYNC_MAX ? count : null;
}
export function useSyncSettings(api, paint = afterPaint) {
  const life = useLifetime();
  const count = ref(SYNC_DEFAULT), draft = ref(String(SYNC_DEFAULT));
  const ready = ref(false), busy = ref(false), error = ref(''), message = ref('');
  function reply(result) {
    let value;
    try { value = JSON.parse(result.stdout); } catch { /* failed or old backend */ }
    if (!result || result.code || value?.status !== 'ok' || !Number.isInteger(value.concurrency) || concurrencyNumber(value.concurrency) === null)
      throw new Error(value?.message || '无法核实同步设置，请重新读取或升级完整模块。');
    return value.concurrency;
  }
  async function load() {
    if (!life.active || busy.value) return;
    busy.value = true; error.value = '';
    try {
      const value = reply(await api.command('--sync-settings'));
      if (!life.active) return;
      count.value = value; draft.value = String(value); ready.value = true;
    } catch (e) { if (life.active) { ready.value = false; error.value = e.message; } }
    finally { if (life.active) busy.value = false; }
  }
  async function save() {
    if (!life.active || busy.value || !ready.value) return;
    const value = concurrencyNumber(draft.value);
    error.value = ''; message.value = '';
    if (value === null) { error.value = `请输入 1–${SYNC_MAX} 之间的整数`; return; }
    busy.value = true;
    try {
      await paint();
      if (!life.active) return;
      const saved = reply(await api.command('--sync-concurrency ' + value));
      if (!life.active) return;
      if (saved !== value) throw new Error('保存结果与请求不一致，请重新读取核实。');
      count.value = saved; draft.value = String(saved); message.value = '已保存，下次同步时生效。';
    } catch (e) { if (life.active) error.value = e.message; }
    finally { if (life.active) busy.value = false; }
  }
  return { count, draft, ready, busy, error, message, load, save };
}
