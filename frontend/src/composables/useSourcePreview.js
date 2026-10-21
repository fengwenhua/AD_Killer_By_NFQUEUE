import { computed, onScopeDispose, ref } from 'vue';
import { HAGEZI_ID, lastJson, sourceUrlError } from '../domain/rules.js';
import { shellQuote } from '../services/bridge.js';
import { PREVIEW_DEFAULT, previewCharacters } from './useUiPreferences.js';
import { afterPaint as paint } from '../services/paint.js';

export function useSourcePreview(api, characters = ref(PREVIEW_DEFAULT), afterPaint = paint, now = Date.now) {
  const active = ref(null), cache = new Map(), inflight = new Map(), queue = [];
  let alive = true, running = 0;
  const pending = computed(() => !!active.value?.items.some(item => item.pending));
  const keyFor = (url, count) => `${count}:${url}`;
  function cached(url, count) {
    const key = keyFor(url, count), entry = cache.get(key);
    if (!entry || now() - entry.at > 300000) { cache.delete(key); return null; }
    cache.delete(key); cache.set(key, entry); return entry;
  }
  async function fetch(url, count) {
    try {
      const result = await api.command(`--source-preview ${shellQuote(url)} ${count}`);
      const data = lastJson(result.stdout), item = data?.previews?.find(item => item.url === url);
      if (!item) return { error: data?.message || result.stderr?.trim() || '无法读取预览，请确认已升级模块，或稍后重试。' };
      if (!item.error && (result.code !== 0 || data.status !== 'ok'))
        return { error: data?.message || result.stderr?.trim() || '来源请求未成功，请稍后重试。' };
      const text = Array.from(String(item.text || ''));
      return { text: text.slice(0, count).join(''), error: String(item.error || ''),
        truncated: !!item.truncated || text.length > count, at: now() };
    } catch (error) { return { error: error?.message || '读取失败，请稍后重试。' }; }
  }
  function pump() {
    while (alive && running < 2 && queue.length) {
      const job = queue.shift(); ++running;
      void fetch(job.url, job.count).then(result => {
        if (alive && !result.error) {
          cache.set(job.key, result);
          while (cache.size > 16) cache.delete(cache.keys().next().value);
        }
        job.resolve(result);
      }).finally(() => { --running; inflight.delete(job.key); pump(); });
    }
  }
  function request(url, count) {
    const key = keyFor(url, count);
    if (inflight.has(key)) return inflight.get(key);
    const promise = new Promise(resolve => queue.push({ key, url, count, resolve }));
    inflight.set(key, promise); pump(); return promise;
  }
  async function load(current, force) {
    // The dialog and its loading state get a painted frame BEFORE touching a
    // native bridge (some managers can briefly block inside the Java call).
    await afterPaint();
    if (!alive || active.value !== current) return;
    await Promise.all(current.items.map(async item => {
      if (item.error || (!item.pending && !force)) return;
      const result = await request(item.url, current.characters);
      if (alive) Object.assign(item, result, { pending: false });
    }));
  }
  function open(source, force = false) {
    const count = previewCharacters(characters.value) ?? PREVIEW_DEFAULT;
    const builtin = source.url === HAGEZI_ID;
    const urls = builtin ? source.dependencies : [source.url];
    active.value = { url: source.url, builtin, characters: count, items: (urls?.length ? urls : [source.url]).map((url, index) => {
      const error = sourceUrlError(url) || (url === HAGEZI_ID ? '内置来源缺少下载地址，请重新加载来源列表。' : '');
      if (force) cache.delete(keyFor(url, count));
      const entry = !force && !error ? cached(url, count) : null;
      return { url, label: builtin ? (index === 0 ? 'Pro' : 'Fake') : '', text: '', truncated: false,
        error, at: null, ...entry, pending: !error && !entry };
    }) };
    return load(active.value, force);
  }
  const close = () => {
    active.value = null;
    // Closing never kills a shared native request, but work that has not
    // started should not delay the next source the user actually wants.
    for (const job of queue.splice(0)) {
      inflight.delete(job.key); job.resolve({ error: '预览已关闭' });
    }
  };
  const refresh = () => active.value && open({ url: active.value.url, dependencies: active.value.items.map(item => item.url) }, true);
  onScopeDispose(() => {
    alive = false; active.value = null; cache.clear();
    for (const job of queue.splice(0)) job.resolve({ error: '页面已关闭' });
    inflight.clear();
  });
  return { active, pending, open, close, refresh };
}
