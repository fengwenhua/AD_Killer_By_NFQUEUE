<script setup>
import { computed, reactive, ref } from 'vue';
import ActionButton from '../components/ActionButton.vue';
import { useVirtualApps } from '../composables/useVirtualApps.js';
const props = defineProps({ apps: Object, engine: Object });
const list = ref(null);
const virtual = reactive(useVirtualApps(computed(() => props.apps.filtered), list));
</script>

<template>
  <div class="view-content">
    <div class="page-heading"><h1>应用名单</h1></div>
    <div class="card">
      <div class="section-heading"><h2 id="appTitle">应用{{ apps.noun }}名单</h2><span id="appCount" class="badge">{{ apps.selected.size }} 个{{ apps.noun }}</span></div>
      <p id="appHelp" class="subtle">{{ engine.scope === 1 ? '仅名单内应用的流量由拦截引擎处理' : '名单内应用的流量不经过拦截引擎' }}</p>
      <p class="subtle">当前编辑的是{{ apps.noun }}名单，两种范围分别保存、不互相复制。勾选只是草稿，请点击“保存并应用”；共享 UID 的应用会一起生效。</p>
      <label class="sr-only" for="appSearch">搜索应用名称或包名</label>
      <input id="appSearch" v-model="apps.query" class="search" type="search" placeholder="搜索应用名称 / 包名" autocomplete="off">
      <div id="appList" ref="list" class="app-list" :aria-busy="apps.loading" :data-total="apps.filtered.length" @scroll.passive="virtual.scroll" @keydown="virtual.keydown">
        <p v-if="apps.loading" class="empty-state" role="status">正在读取已安装应用…</p>
        <template v-else>
          <div v-if="virtual.before" class="virtual-space" :style="{ height: virtual.before + 'px' }" aria-hidden="true" />
          <label v-for="(app, offset) in virtual.visible" :key="app.pkg" class="app" :style="{ height: virtual.rowHeight + 'px' }">
            <span class="info"><b :title="app.label">{{ app.label }}</b><span :title="app.pkg">{{ app.pkg }}</span></span>
            <input type="checkbox" :data-app-index="virtual.start + offset" :checked="apps.selected.has(app.pkg)" :disabled="apps.saving || !!engine.setting" @change="apps.toggle(app.pkg, $event.target.checked)">
          </label>
          <div v-if="virtual.after" class="virtual-space" :style="{ height: virtual.after + 'px' }" aria-hidden="true" />
        </template>
        <p v-if="!apps.loading && !apps.filtered.length" class="empty-state">没有匹配的应用。管理器不支持应用目录时，仅显示已保存名单。</p>
      </div>
      <div class="row app-actions"><ActionButton :disabled="!apps.ready || apps.loading || apps.saving" @click="apps.clear">清空选择</ActionButton><ActionButton id="saveApps" variant="primary" :busy="apps.saving" :disabled="!apps.ready || apps.loading || !!engine.setting || !!engine.action" @click="apps.save">{{ apps.saving ? '正在保存…' : '保存并应用' }}</ActionButton></div>
    </div>
  </div>
</template>
