import { computed, ref, watch } from 'vue';
import { PATHS } from '../services/api.js';
import { isPackage } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';

export function useApps(api, feedback, scope) {
  const life = useLifetime();
  const catalog = ref([]), selected = ref(new Set()), query = ref(''), loading = ref(false), saving = ref(false), ready = ref(false);
  const drafts = new Map();
  let epoch = 0;
  const noun = computed(() => scope.value === 1 ? '拦截' : '放行');
  const filtered = computed(() => catalog.value.filter(app => `${app.label} ${app.pkg}`.toLowerCase().includes(query.value.toLowerCase())));
  const pathFor = current => current === 1 ? PATHS.intercept : PATHS.allow;
  async function load() {
    const current = scope.value, token = ++epoch;
    loading.value = true;
    let selection = drafts.get(current);
    if (!selection) selection = new Set((await api.readFile(pathFor(current))).split(/\r?\n/).map(s => s.trim()).filter(isPackage));
    if (!life.active || token !== epoch) return;
    selected.value = new Set(selection); drafts.set(current, selected.value);
    let entries = [...selection].map(pkg => ({ pkg, label: pkg }));
    const manager = api.packageManager();
    try {
      if (manager?.listPackages && manager?.getPackagesInfo) {
        const packages = JSON.parse(await manager.listPackages('all'));
        const all = [...new Set([...(Array.isArray(packages) ? packages.filter(isPackage) : []), ...selection])];
        const info = new Map();
        for (let i = 0; i < all.length && life.active && token === epoch; i += 80) {
          try {
            const batch = JSON.parse(await manager.getPackagesInfo(JSON.stringify(all.slice(i, i + 80))));
            if (Array.isArray(batch)) batch.forEach(item => { if (item && isPackage(item.packageName)) info.set(item.packageName, item); });
          } catch { /* show package names if the manager cannot resolve labels */ }
        }
        entries = all.map(pkg => ({ pkg, label: String(info.get(pkg)?.appLabel || pkg) }));
      }
    } catch { feedback.log('无法读取完整应用目录，仅显示已保存的应用名单。', 'error'); }
    if (!life.active || token !== epoch) return;
    catalog.value = entries.sort((a, b) => Number(selection.has(b.pkg)) - Number(selection.has(a.pkg)) || a.label.localeCompare(b.label, 'zh-Hans-CN'));
    loading.value = false; ready.value = true;
  }
  watch(scope, () => { if (ready.value || loading.value) void load(); });
  function toggle(pkg, enabled) {
    if (saving.value || loading.value || !isPackage(pkg)) return;
    enabled ? selected.value.add(pkg) : selected.value.delete(pkg);
    drafts.set(scope.value, selected.value);
  }
  function clear() {
    if (saving.value || loading.value) return;
    selected.value.clear(); drafts.set(scope.value, selected.value); feedback.toast('名单已清空（尚未保存）');
  }
  async function save() {
    if (!ready.value || saving.value || loading.value) return;
    saving.value = true;
    const current = scope.value, packages = [...selected.value].filter(isPackage);
    try {
      const write = await api.writeFile(pathFor(current), packages.join('\n') + (packages.length ? '\n' : ''));
      if (write.code) { feedback.log('应用名单写入失败：' + write.stderr, 'error'); feedback.toast('保存失败'); return; }
      const result = await api.command('--apps-apply');
      const text = result.code ? '名单已保存，但应用路由刷新失败，请重试' : result.stdout.includes('deferred') ? '名单已保存，启动后生效' : '应用名单已保存，已提交后台热重载；失败原因请查看引擎日志';
      feedback.log(text, result.code ? 'error' : 'ok'); feedback.toast(text);
    } finally { saving.value = false; }
  }
  return { catalog, selected, query, loading, saving, ready, noun, filtered, load, toggle, clear, save };
}
