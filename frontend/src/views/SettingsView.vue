<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue';
import UiIcon from '../components/UiIcon.vue';
import ActionButton from '../components/ActionButton.vue';
import SettingSwitch from '../components/SettingSwitch.vue';
import { PREVIEW_MIN, PREVIEW_MAX } from '../composables/useUiPreferences.js';
const props = defineProps({ moduleInfo: Object, preferences: Object, updates: Object });
const active = ref(''), root = ref(null), heading = ref(null);
const scrolled = ref(false);
let scrollPane;
const trackScroll = () => { scrolled.value = (scrollPane?.scrollTop || 0) > 8; };
onMounted(() => {
  scrollPane = root.value.closest('.page');
  scrollPane.addEventListener('scroll', trackScroll, { passive: true });
});
onBeforeUnmount(() => scrollPane?.removeEventListener('scroll', trackScroll));
const entries = [
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
          <span class="settings-entry-value">{{ entry.id === 'preview' ? preferences.characters + ' 字符' : entry.id === 'updates' ? (updates.checking ? '检查中…' : updates.available ? '发现新版本' : '手动检查') : entry.id === 'releases' ? '最新发布' : 'v' + moduleInfo.version }}</span>
          <UiIcon name="chevron" />
        </button>
      </template>
    </div>
    <div v-show="!!active" class="settings-detail">
      <section v-show="active === 'preview'" id="settings-preview" class="card preview-settings" aria-labelledby="previewSettingsTitle">
        <h2 id="previewSettingsTitle">内容预览</h2>
        <form novalidate @submit.prevent="preferences.save">
          <label for="previewCharacters">每个来源的预览字符数</label>
          <p id="previewCharactersHelp" class="subtle">默认 500，可设置 {{ PREVIEW_MIN }}–{{ PREVIEW_MAX }} 个字符。仅影响只读预览，不影响规则同步。</p>
          <div class="row"><input id="previewCharacters" v-model="preferences.draft" class="search" type="number" inputmode="numeric" :min="PREVIEW_MIN" :max="PREVIEW_MAX" step="1" :aria-invalid="!!preferences.error" aria-describedby="previewCharactersHelp previewSettingsStatus"><button id="savePreviewSettings" type="submit" class="btn primary">保存设置</button></div>
          <p id="previewSettingsStatus" class="sync-note" :data-kind="preferences.error ? 'error' : 'success'" role="status">{{ preferences.error || preferences.message }}</p>
        </form>
      </section>
      <section v-show="active === 'updates'" id="settings-updates" class="card update-card" aria-labelledby="updateTitle">
        <div class="section-heading"><h2 id="updateTitle">检查更新</h2><span class="badge">v{{ moduleInfo.version }}</span></div>
        <p class="subtle update-intro">检查新版本，下载校验后由你确认安装。</p>
        <SettingSwitch id="updatePreview" label="包含预览版" help="关闭时只检查正式版；预览版可能尚不稳定" :checked="updates.preview" :disabled="updates.busy || updates.checking" @change="updates.setPreview" />
        <div class="update-status" :data-kind="updates.kind" :aria-busy="updates.checking || updates.busy">
          <span class="update-status-icon"><UiIcon :name="updates.reboot ? 'info' : updates.available ? 'download' : 'shield'" /></span>
          <div><b>{{ updates.reboot ? '等待重启' : updates.checking ? '正在检查' : updates.busy ? '正在处理' : updates.available ? '有新版本可用' : '版本检查' }}</b><p id="moduleUpdateStatus" role="status">{{ updates.message }}</p></div>
        </div>
        <div class="update-actions">
          <ActionButton id="getUpdate" icon="sync" variant="primary" :busy="updates.checking" :disabled="updates.busy" @click="updates.check">{{ updates.checking ? '检查中…' : '检查更新' }}</ActionButton>
          <ActionButton v-if="updates.available && !updates.reboot" id="downloadModuleUpdate" icon="download" :busy="updates.busy && !updates.canInstall" :disabled="updates.busy || updates.checking" @click="updates.download">下载并校验</ActionButton>
          <ActionButton v-if="updates.canInstall && !updates.confirming" id="installModuleUpdate" icon="download" :busy="updates.busy" :disabled="updates.busy || updates.checking" @click="updates.confirming = true">安装 v{{ updates.ready }}</ActionButton>
        </div>
        <div v-if="updates.confirming" class="update-confirm" role="group" aria-label="确认安装模块">
          <p>将通过根管理器安装 v{{ updates.ready }}，安装完成后需手动重启设备。</p>
          <div class="update-actions"><ActionButton id="confirmModuleInstall" variant="primary" :busy="updates.busy" @click="updates.install">确认安装</ActionButton><button type="button" class="btn" :disabled="updates.busy" @click="updates.confirming = false">取消</button></div>
        </div>
      </section>
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
        <p v-if="updates.notesLoading" class="sync-note" role="status">正在读取 GitHub 发布日志…</p>
        <div v-else-if="updates.notesError" class="release-error"><p class="sync-note" data-kind="error" role="status">{{ updates.notesError }}</p></div>
        <ol v-if="updates.latestRelease && !updates.notesLoading" class="release-list">
          <li><div class="release-heading"><b>v{{ updates.latestRelease.version }}</b><span class="badge">{{ updates.latestRelease.preview ? '预览版' : '正式版' }}</span><time v-if="updates.latestRelease.date">{{ updates.latestRelease.date }}</time></div><h3>{{ updates.latestRelease.title }}</h3><pre class="release-body">{{ updates.latestRelease.body || '本次发布未填写更新说明。' }}</pre></li>
        </ol>
        <div class="release-actions"><ActionButton id="reloadReleaseNotes" icon="sync" :busy="updates.notesLoading" @click="reloadNotes">{{ updates.notesLoading ? '加载中…' : '重新加载' }}</ActionButton></div>
      </section></div>
    </div>
  </div>
</template>
