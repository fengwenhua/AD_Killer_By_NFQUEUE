<script setup>
import { onMounted, reactive, ref } from 'vue';
import { createApi } from './services/api.js';
import { useEngine } from './composables/useEngine.js';
import { useSources } from './composables/useSources.js';
import { useSync } from './composables/useSync.js';
import { useApps } from './composables/useApps.js';
import { useCustomRules } from './composables/useCustomRules.js';
import { useFeedback } from './composables/useFeedback.js';
import { useTheme } from './composables/useTheme.js';
import { useSourcePreview } from './composables/useSourcePreview.js';
import { useModuleInfo } from './composables/useModuleInfo.js';
import { useNavigationMotion } from './composables/useNavigationMotion.js';
import { useUiPreferences } from './composables/useUiPreferences.js';
import { useModuleUpdate } from './composables/useModuleUpdate.js';
import SourcePreview from './components/SourcePreview.vue';
import UiIcon from './components/UiIcon.vue';
import HomeView from './views/HomeView.vue';
import AppsView from './views/AppsView.vue';
import RulesView from './views/RulesView.vue';
import SettingsView from './views/SettingsView.vue';

const api = createApi(), feedbackRaw = useFeedback(), engineRaw = useEngine(api, feedbackRaw);
const sourcesRaw = useSources(api, feedbackRaw);
const feedback = reactive(feedbackRaw), engine = reactive(engineRaw), sources = reactive(sourcesRaw);
const apps = reactive(useApps(api, feedbackRaw, engineRaw.scope));
const custom = reactive(useCustomRules(api, feedbackRaw));
const preferencesRaw = useUiPreferences(), preferences = reactive(preferencesRaw);
const previews = reactive(useSourcePreview(api, preferencesRaw.characters));
const sync = reactive(useSync(api, feedbackRaw, sourcesRaw.history, engineRaw.refresh));
const { theme, toggle: toggleTheme } = useTheme();
const moduleInfoRaw = useModuleInfo(api), moduleInfo = reactive(moduleInfoRaw);
const moduleUpdate = reactive(useModuleUpdate(api,moduleInfoRaw,feedbackRaw));
const page = ref('home'), mainElement = ref(null), navigationElement = ref(null), themeButton = ref(null);
const { leaving, selected, navigate } = useNavigationMotion(page, mainElement, theme, themeButton);
const pages = [{ id: 'home', label: '概览', icon: 'home' }, { id: 'apps', label: '应用名单', icon: 'apps' }, { id: 'rules', label: '规则管理', icon: 'rules' }, { id: 'settings', label: '设置', icon: 'settings' }];
onMounted(async () => {
  feedback.log('AD_Killer_By_NFQUEUE 控制台已就绪');
  await Promise.all([engine.refresh(), custom.load(), sources.load(), moduleInfo.load()]);
  void moduleUpdate.resume();
  await apps.load();
  await sync.resume();
  feedback.log(engine.ready ? '已连接引擎接口' : '未连接引擎接口，请在管理器中打开页面', engine.ready ? 'ok' : 'error');
});
</script>

<template>
  <a class="skip-link" href="#main-content">跳到主要内容</a>
  <header class="appbar">
    <span class="brand-mark"><UiIcon name="shield" /></span>
    <div class="brand-copy"><b>AD_Killer_By_NFQUEUE<span class="brand-dot">.</span></b></div>
    <button ref="themeButton" class="theme-toggle" type="button" :aria-label="theme === 'dark' ? '切换为浅色主题' : '切换为深色主题'" @click="toggleTheme"><UiIcon :name="theme === 'dark' ? 'sun' : 'moon'" /></button>
  </header>
  <nav ref="navigationElement" class="floating-nav" aria-label="主导航" :style="{ '--tab-index': pages.findIndex(item => item.id === selected) }">
    <span class="nav-indicator" aria-hidden="true" />
    <button v-for="item in pages" :key="item.id" type="button" :data-page="item.id" :class="{ active: selected === item.id }" :aria-current="selected === item.id ? 'page' : undefined" :aria-controls="item.id" @click="navigate(item.id)"><UiIcon :name="item.icon" /><span>{{ item.label }}</span></button>
  </nav>
  <main id="main-content" ref="mainElement" class="wrap page-stage" :class="{ sliding: leaving.length > 0 }" tabindex="-1">
    <!-- Keep pages mounted: folding, rotation and navigation retain drafts,
         search, native details state and in-flight job observers. -->
    <section id="home" class="page" aria-label="概览" :data-active="page === 'home' || undefined" :data-leaving="leaving.includes('home') || undefined" :inert="page !== 'home'"><HomeView :engine="engine" :sync="sync" :apps="apps" :feedback="feedback" :module-info="moduleInfo" /></section>
    <section id="apps" class="page" aria-label="应用名单" :data-active="page === 'apps' || undefined" :data-leaving="leaving.includes('apps') || undefined" :inert="page !== 'apps'"><AppsView :apps="apps" :engine="engine" /></section>
    <section id="rules" class="page" aria-label="规则管理" :data-active="page === 'rules' || undefined" :data-leaving="leaving.includes('rules') || undefined" :inert="page !== 'rules'"><RulesView :sources="sources" :custom="custom" :previews="previews" /></section>
    <section id="settings" class="page" aria-label="设置" :data-active="page === 'settings' || undefined" :data-leaving="leaving.includes('settings') || undefined" :inert="page !== 'settings'"><SettingsView :module-info="moduleInfo" :preferences="preferences" :updates="moduleUpdate" /></section>
  </main>
  <SourcePreview :model="previews" />
  <div id="toast" role="status" :class="{ show: feedback.toastText }">{{ feedback.toastText }}</div>
</template>
