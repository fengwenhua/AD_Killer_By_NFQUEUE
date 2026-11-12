<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue';
import UiIcon from '../components/UiIcon.vue';
import ActionButton from '../components/ActionButton.vue';
import SettingSwitch from '../components/SettingSwitch.vue';
import ModuleUpdatePanel from '../components/ModuleUpdatePanel.vue';
import { PREVIEW_MIN, PREVIEW_MAX } from '../composables/useUiPreferences.js';
import { SYNC_MAX, SYNC_INTERVAL_MAX, SYNC_RETRIES_MAX } from '../composables/useSyncSettings.js';
const props = defineProps({ moduleInfo: Object, preferences: Object, updates: Object, syncSettings: Object });
const active = ref(''), root = ref(null), heading = ref(null);
const scrolled = ref(false);
let scrollPane;
const cacheTime = value => new Date(value).toLocaleString('zh-CN', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' });
const trackScroll = () => { scrolled.value = (scrollPane?.scrollTop || 0) > 8; };
onMounted(() => {
  scrollPane = root.value.closest('.page');
  scrollPane.addEventListener('scroll', trackScroll, { passive: true });
});
onBeforeUnmount(() => scrollPane?.removeEventListener('scroll', trackScroll));
const entries = [
  { id: 'sync', title: '规则同步', icon: 'sync' },
  { id: 'preview', title: '内容预览', icon: 'rules' },
  { id: 'updates', title: '检查更新', icon: 'download' },
  { id: 'releases', title: '更新日志', icon: 'sync' },
  { id: 'about', title: '关于模块', icon: 'info' },
];
const positions = new Map();
let lastEntry = '', revision = 0;
async function reloadNotes(event) {
  const button = event.currentTarget;
  const focused = document.activeElement === button;
  await props.updates.loadReleaseNotes(true);
  await nextTick();
  // Chromium versions differ on whether disabling a focused button blurs it.
  // Restore only if the user has not moved focus or left this settings detail.
  if (focused && active.value === 'releases' && root.value && button.isConnected &&
      (document.activeElement === document.body || document.activeElement === button)) button.focus({ preventScroll: true });
}
async function show(id) {
  if (id && !entries.some(entry => entry.id === id)) return;
  const pane = root.value?.closest('.page');
  if (!pane || active.value === id) return;
  positions.set(active.value, pane.scrollTop);
  if (id) lastEntry = id;
  active.value = id;
  if (id === 'releases') void props.updates.loadReleaseNotes();
  const current = ++revision;
  await nextTick();
  if (current !== revision || !root.value) return;
  pane.scrollTop = positions.get(id) || 0;
  trackScroll();
  const focus = id ? heading.value : root.value.querySelector(`[data-settings-entry="${lastEntry}"]`);
  focus?.focus({ preventScroll: true });
}
</script>

<template>
  <div ref="root" class="view-content" @keydown.esc="active && show('')">
    <div class="page-heading settings-heading" :class="{ 'settings-detail-heading': !!active, 'is-scrolled': !!active && scrolled }">
      <button v-if="active" id="settingsBack" type="button" class="settings-back" aria-label="返回设置" @click="show('')"><UiIcon name="back" /><span>设置</span></button>
      <span v-if="active" class="settings-separator" aria-hidden="true">/</span>
      <h1 ref="heading" tabindex="-1">{{ entries.find(entry => entry.id === active)?.title || '设置' }}</h1>
    </div>
    <div v-show="!active" id="settingsMenu" class="card settings-menu" role="group" aria-label="设置项目">
      <template v-for="entry in entries" :key="entry.id">
        <SettingSwitch v-if="entry.id === 'updates'" id="autoCheckUpdate" class="settings-entry settings-auto" label="自动检查更新" help="打开界面时检查新版本" :checked="updates.autoCheck" @change="updates.setAutoCheck"><template #leading><span class="settings-entry-icon"><UiIcon name="sync" /></span></template></SettingSwitch>
        <button type="button" class="settings-entry" :data-settings-entry="entry.id" :aria-controls="'settings-' + entry.id" @click="show(entry.id)">
          <span class="settings-entry-icon"><UiIcon :name="entry.icon" /></span>
          <span class="settings-entry-label">{{ entry.title }}</span>
          <span class="settings-entry-value" :data-update="entry.id === 'updates' && updates.available || undefined">{{ entry.id === 'sync' ? (syncSettings.ready ? syncSettings.count + ' 并发 · ' + syncSettings.interval + ' 分钟' : '未读取') : entry.id === 'preview' ? preferences.characters + ' 字符' : entry.id === 'updates' ? (updates.checking ? '检查中…' : updates.available ? '有更新' : '手动检查') : entry.id === 'releases' ? '最新发布' : 'v' + moduleInfo.version }}</span>
          <UiIcon name="chevron" />
        </button>
      </template>
    </div>
    <div v-show="!!active" class="settings-detail">
      <section v-show="active === 'sync'" id="settings-sync" class="card preview-settings" aria-labelledby="syncSettingsTitle">
        <h2 id="syncSettingsTitle">规则同步</h2>
        <form novalidate @submit.prevent="syncSettings.save">
          <label for="syncConcurrency">同步并发数量</label>
          <p id="syncConcurrencyHelp" class="subtle">默认 10，可设置 1–{{ SYNC_MAX }}。多个来源同时下载，全部校验通过后一次性更新规则。网络较慢或设备资源紧张时可调低。</p>
          <input id="syncConcurrency" v-model="syncSettings.draft" class="search" type="number" inputmode="numeric" min="1" :max="SYNC_MAX" step="1" :disabled="!syncSettings.ready || syncSettings.busy" aria-describedby="syncConcurrencyHelp syncSettingsStatus">
          <div class="setting-field"><label for="syncInterval">同步时间间隔（分钟）</label><p id="syncIntervalHelp" class="subtle">默认 30。再次手动同步时，间隔内复用成功来源的缓存，失败来源继续重试；0 表示每次重新下载。</p><input id="syncInterval" v-model="syncSettings.intervalDraft" class="search" type="number" inputmode="numeric" min="0" :max="SYNC_INTERVAL_MAX" step="1" :disabled="!syncSettings.ready || syncSettings.busy" aria-describedby="syncIntervalHelp syncSettingsStatus"></div>
          <div class="setting-field"><label for="syncRetries">失败重试次数</label><p id="syncRetriesHelp" class="subtle">默认 2，即首次失败后最多再试 2 次。每个来源独立重试；0 表示不重试。</p><input id="syncRetries" v-model="syncSettings.retriesDraft" class="search" type="number" inputmode="numeric" min="0" :max="SYNC_RETRIES_MAX" step="1" :disabled="!syncSettings.ready || syncSettings.busy" aria-describedby="syncRetriesHelp syncSettingsStatus"></div>
          <div class="setting-field"><ActionButton id="saveSyncSettings" variant="primary" :busy="syncSettings.busy" :disabled="!syncSettings.ready" @click="syncSettings.save">{{ syncSettings.busy ? '保存中…' : '保存设置' }}</ActionButton></div>
          <p id="syncSettingsStatus" class="sync-note" :data-kind="syncSettings.error ? 'error' : 'success'" role="status">{{ syncSettings.error || syncSettings.message }}</p>
          <button v-if="!syncSettings.ready" type="button" class="btn" :disabled="syncSettings.busy" @click="syncSettings.load">重新读取</button>
        </form>
      </section>
      <section v-show="active === 'preview'" id="settings-preview" class="card preview-settings" aria-labelledby="previewSettingsTitle">
        <h2 id="previewSettingsTitle">内容预览</h2>
        <form novalidate @submit.prevent="preferences.save">
          <label for="previewCharacters">每个来源的预览字符数</label>
          <p id="previewCharactersHelp" class="subtle">默认 500，可设置 {{ PREVIEW_MIN }}–{{ PREVIEW_MAX }} 个字符。仅影响只读预览，不影响规则同步。</p>
          <div class="row"><input id="previewCharacters" v-model="preferences.draft" class="search" type="number" inputmode="numeric" :min="PREVIEW_MIN" :max="PREVIEW_MAX" step="1" :aria-invalid="!!preferences.error" aria-describedby="previewCharactersHelp previewSettingsStatus"><button id="savePreviewSettings" type="submit" class="btn primary">保存设置</button></div>
          <p id="previewSettingsStatus" class="sync-note" :data-kind="preferences.error ? 'error' : 'success'" role="status">{{ preferences.error || preferences.message }}</p>
        </form>
      </section>
      <ModuleUpdatePanel v-show="active === 'updates'" :active="active === 'updates'" :updates="updates" :module-info="moduleInfo" />
      <div v-show="active === 'about'" id="settings-about"><section id="aboutModule" class="card about-card" aria-labelledby="aboutTitle">
        <div class="section-heading"><h2 id="aboutTitle">关于模块</h2><UiIcon name="info" /></div>
        <div class="about-identity"><span class="about-mark"><UiIcon name="shield" /></span><div class="about-brand"><h3>AD Killer</h3><span class="brand-subtitle">by NFQUEUE</span></div></div>
        <p class="about-description">系统级域名过滤模块，采用 Linux NFQUEUE 与本地 C++ 引擎。</p>
        <dl class="module-facts">
          <div><dt>模块版本</dt><dd id="aboutVersion">v{{ moduleInfo.version }} <span class="subtle">{{ moduleInfo.releaseChannel === 'preview' ? '预览版' : moduleInfo.releaseChannel === 'stable' ? '正式版' : '渠道未知' }}</span></dd></div>
          <div><dt>作者</dt><dd><a href="https://github.com/fengwenhua" target="_blank" rel="noopener noreferrer">fengwenhua <UiIcon name="external" /></a></dd></div>
          <div><dt>致谢</dt><dd><a href="https://yhyun.asia/" target="_blank" rel="noopener noreferrer">感谢 YH_Hosts <UiIcon name="external" /></a></dd></div>
          <div><dt>数据目录</dt><dd><code>/data/adb/AD_Killer_By_NFQUEUE/</code></dd></div>
        </dl>
        <p v-if="moduleInfo.mismatch" id="versionMismatch" class="sync-note" data-kind="warning" role="status">模块与界面版本不一致，请重新安装匹配的完整模块包。</p>
        <p v-else-if="!moduleInfo.verified" id="moduleUnverified" class="sync-note" data-kind="warning">尚未核实已安装模块，暂时显示随包版本。</p>
        <a class="github-link" href="https://github.com/fengwenhua/AD_Killer_By_NFQUEUE" target="_blank" rel="noopener noreferrer"><span class="github-mark"><UiIcon name="github" /></span><span><b>GitHub</b><small>在GitHub上查看</small></span><UiIcon name="external" /></a>
        <div class="about-footer">本项目代码 MIT · Vue MIT · GSAP Standard License</div>
      </section></div>
      <div v-show="active === 'releases'" id="settings-releases"><section id="releaseNotes" class="card release-card" aria-labelledby="releaseTitle">
        <div class="section-heading"><h2 id="releaseTitle">更新日志</h2><span class="badge">GitHub Release</span></div>
        <p class="subtle">只显示最新一次发布，包含预览版。</p>
        <p v-if="updates.notesUpdatedAt" class="sync-note">{{ updates.notesCached ? '已缓存 · ' : '读取于 · ' }}{{ cacheTime(updates.notesUpdatedAt) }}，可手动重新加载。</p>
        <p v-if="updates.notesLoading" class="sync-note" role="status">正在读取 GitHub 发布日志…</p>
        <div v-else-if="updates.notesError" class="release-error"><p class="sync-note" data-kind="error" role="status">{{ updates.notesError }}</p></div>
        <ol v-if="updates.latestRelease && !updates.notesLoading" class="release-list">
          <li><div class="release-heading"><b>v{{ updates.latestRelease.version }}</b><span class="badge">{{ updates.latestRelease.preview ? '预览版' : '正式版' }}</span><time v-if="updates.latestRelease.date">{{ updates.latestRelease.date }}</time></div><h3>{{ updates.latestRelease.title }}</h3><pre class="release-body">{{ updates.latestRelease.body || '本次发布未填写更新说明。' }}</pre></li>
        </ol>
        <p v-if="updates.latestRelease?.truncated && !updates.notesLoading" class="sync-note">日志较长，仅显示开头部分。完整内容可在 GitHub 查看。</p>
        <div class="release-actions"><ActionButton id="reloadReleaseNotes" icon="sync" :busy="updates.notesLoading" @click="reloadNotes">{{ updates.notesLoading ? '加载中…' : '重新加载' }}</ActionButton></div>
      </section></div>
    </div>
  </div>
</template>
