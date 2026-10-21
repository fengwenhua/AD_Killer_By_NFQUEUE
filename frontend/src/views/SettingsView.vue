<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue';
import UiIcon from '../components/UiIcon.vue';
import ActionButton from '../components/ActionButton.vue';
import { releaseNotes } from '../domain/module.js';
import { PREVIEW_MIN, PREVIEW_MAX } from '../composables/useUiPreferences.js';
import { RELEASES_URL } from '../domain/updates.js';
const props = defineProps({ moduleInfo: Object, preferences: Object, updates: Object });
const releases = releaseNotes(props.moduleInfo.build);
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
  { id: 'releases', title: '更新日志', icon: 'sync' },
  { id: 'about', title: '关于模块', icon: 'info' },
];
const positions = new Map();
let lastEntry = '', revision = 0;
async function show(id) {
  if (id && !entries.some(entry => entry.id === id)) return;
  const pane = root.value?.closest('.page');
  if (!pane || active.value === id) return;
  positions.set(active.value, pane.scrollTop);
  if (id) lastEntry = id;
  active.value = id;
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
      <button v-for="entry in entries" :key="entry.id" type="button" class="settings-entry" :data-settings-entry="entry.id" :aria-controls="'settings-' + entry.id" @click="show(entry.id)">
        <span class="settings-entry-icon"><UiIcon :name="entry.icon" /></span>
        <span class="settings-entry-label">{{ entry.title }}</span>
        <span class="settings-entry-value">{{ entry.id === 'preview' ? preferences.characters + ' 字符' : entry.id === 'releases' ? '最近 ' + releases.length + ' 次' : 'v' + moduleInfo.version }}</span>
        <UiIcon name="chevron" />
      </button>
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
      <div v-show="active === 'about'" id="settings-about"><section id="aboutModule" class="card about-card" aria-labelledby="aboutTitle">
        <div class="section-heading"><h2 id="aboutTitle">关于模块</h2><UiIcon name="info" /></div>
        <div class="about-identity"><span class="about-mark"><UiIcon name="shield" /></span><h3>AD_Killer_By_NFQUEUE</h3></div>
        <p class="about-description">系统级域名过滤模块，采用 Linux NFQUEUE 与本地 C++ 引擎。</p>
        <dl class="module-facts">
          <div><dt>模块版本</dt><dd id="aboutVersion">v{{ moduleInfo.version }} <span class="subtle">({{ moduleInfo.versionCode }})</span></dd></div>
          <div><dt>版本来源</dt><dd id="versionSource">{{ moduleInfo.verified ? '已读取本机模块' : '随包界面版本 · 未核实引擎' }}</dd></div>
          <div><dt>界面版本</dt><dd>v{{ moduleInfo.build.version }}</dd></div>
          <div><dt>作者</dt><dd>{{ moduleInfo.build.author }}</dd></div>
          <div><dt>数据目录</dt><dd><code>/data/adb/AD_Killer_By_NFQUEUE/</code></dd></div>
        </dl>
        <p v-if="moduleInfo.mismatch" id="versionMismatch" class="sync-note" data-kind="warning" role="status">模块与界面版本不一致，请重新安装匹配的完整模块包。</p>
        <div class="module-update">
          <b>模块更新</b>
          <label class="update-channel"><input id="updatePreview" v-model="updates.preview" type="checkbox" :disabled="updates.busy || updates.checking">包含预览版</label>
          <p id="moduleUpdateStatus" class="sync-note" :data-kind="updates.kind" role="status">{{ updates.message }}</p>
          <div class="update-actions">
            <ActionButton id="getUpdate" icon="sync" :disabled="updates.busy || updates.checking" @click="updates.check">{{ updates.checking ? '检查中…' : '检查更新' }}</ActionButton>
            <ActionButton v-if="updates.available && !updates.reboot" id="downloadModuleUpdate" icon="download" :disabled="updates.busy || updates.checking" @click="updates.download">下载并校验</ActionButton>
            <ActionButton v-if="updates.canInstall && !updates.confirming" id="installModuleUpdate" icon="download" :disabled="updates.busy || updates.checking" @click="updates.confirming = true">安装 v{{ updates.ready }}</ActionButton>
          </div>
          <div v-if="updates.confirming" class="update-confirm" role="group" aria-label="确认安装模块">
            <p>将通过根管理器安装 v{{ updates.ready }}，安装完成后需手动重启设备。</p>
            <div class="update-actions"><button id="confirmModuleInstall" type="button" class="btn primary" :disabled="updates.busy" @click="updates.install">确认安装</button><button type="button" class="btn" @click="updates.confirming = false">取消</button></div>
          </div>
          <details v-if="updates.release?.notes"><summary>v{{ updates.release.version }} 发布说明{{ updates.release.preview ? ' · 预览版' : '' }}</summary><pre class="update-notes">{{ updates.release.notes }}</pre></details>
          <a :href="RELEASES_URL" target="_blank" rel="noopener noreferrer">打开 GitHub 发布页 <UiIcon name="external" /></a>
        </div>
        <div class="about-footer"><span>本项目代码 MIT · Vue MIT · GSAP Standard License</span><a href="https://github.com/fengwenhua/AD_Killer_By_NFQUEUE" target="_blank" rel="noopener noreferrer">项目主页 <UiIcon name="external" /></a></div>
      </section></div>
      <div v-show="active === 'releases'" id="settings-releases"><section id="releaseNotes" class="card release-card" aria-labelledby="releaseTitle">
        <div class="section-heading"><h2 id="releaseTitle">更新日志</h2><span class="badge">随包记录</span></div>
        <p class="subtle">本地版本记录，不联网检查新版本。</p>
        <ol class="release-list">
          <li v-for="release in releases" :key="release.version"><div class="release-heading"><b>v{{ release.version }}</b><span v-if="release.current" class="badge">本界面版本</span></div><h3>{{ release.title }}</h3><ul><li v-for="change in release.changes" :key="change">{{ change }}</li></ul></li>
        </ol>
      </section></div>
    </div>
  </div>
</template>
