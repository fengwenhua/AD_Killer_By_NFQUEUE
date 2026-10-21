import { computed, ref } from 'vue';
import { lastJson } from '../domain/rules.js';
import { BUILD_INFO, parseModuleInfo } from '../domain/module.js';
import { useLifetime } from './useLifetime.js';

export function useModuleInfo(api, build = BUILD_INFO) {
  const life = useLifetime(), installed = ref(null), loading = ref(false);
  const version = computed(() => installed.value?.version || build.version);
  const versionCode = computed(() => installed.value?.versionCode || build.versionCode);
  const releaseChannel = computed(() => installed.value?.releaseChannel || build.releaseChannel || 'unknown');
  const verified = computed(() => !!installed.value);
  const mismatch = computed(() => !!installed.value && (installed.value.version !== build.version || installed.value.versionCode !== build.versionCode ||
    (installed.value.releaseChannel !== 'unknown' && build.releaseChannel && installed.value.releaseChannel !== build.releaseChannel)));
  async function load() {
    if (loading.value) return;
    loading.value = true;
    try {
      const result = await api.command('--version-status');
      if (life.active) installed.value = result.code ? null : parseModuleInfo(lastJson(result.stdout));
    } catch { if (life.active) installed.value = null; }
    finally { loading.value = false; }
  }
  return { build, version, versionCode, releaseChannel, verified, mismatch, loading, load };
}
