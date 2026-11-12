<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref, computed } from 'vue';
import SourceCard from '../components/SourceCard.vue';
import ActionButton from '../components/ActionButton.vue';
import SourceEditor from '../components/SourceEditor.vue';
import SyncProgress from '../components/SyncProgress.vue';
import { sourceHistory } from '../domain/rules.js';
const props = defineProps({ sources: Object, custom: Object, previews: Object, sync: Object });
const filter = ref('all');
const needsAttention = row => row.enabled && sourceHistory(row, props.sources.history).needsAction;
const attentionCount = computed(() => props.sources.rows.filter(needsAttention).length);
const rows = computed(() => props.sources.displayRows.filter(row => filter.value === 'all' || (filter.value === 'enabled' ? row.enabled : needsAttention(row))));
const incomplete = computed(() => props.sources.history.some(row => row.publish_status === 'incomplete_sources'));
const editorTrigger = ref(null), editorKey = ref(0);
const sourceKeyboard = ref(false);
function keyboardMode(enabled) {
  if (sourceKeyboard.value === enabled) return;
  const focus = document.activeElement, pane = document.getElementById('rules');
  const anchor = focus?.closest('.source-item') || (pane?.contains(focus) ? focus : null);
  const top = anchor?.getBoundingClientRect().top;
  sourceKeyboard.value = enabled;
  void nextTick(() => {
    if (anchor?.isConnected) pane.scrollTop += anchor.getBoundingClientRect().top - top;
  });
}
const pointer = () => keyboardMode(false);
onMounted(() => document.addEventListener('pointerdown', pointer, { capture: true, passive: true }));
onBeforeUnmount(() => document.removeEventListener('pointerdown', pointer, true));
function edit(row, trigger) {
  const previous = props.sources.editor;
  props.sources.edit(row);
  if (props.sources.editor && props.sources.editor !== previous) {
    editorTrigger.value = trigger || document.activeElement; editorKey.value++;
  }
}
function remove(row) { if (confirm('删除这个来源？当前已加载规则不会立即改变，需保存并再次同步。\n' + row.url)) props.sources.remove(row); }
</script>

<template>
  <div class="view-content" @keydown.capture="keyboardMode(true)">
    <div class="page-heading"><h1>规则管理</h1></div>
    <div class="rules-grid">
      <details id="sourcesDetails" class="card rule-section" :class="{ dirty: sources.dirty }">
        <summary><span>开源规则来源</span><span id="sourceCount" class="badge">{{ sources.enabledCount }} / {{ sources.rows.length }} 启用</span></summary>
        <div class="rule-section-body">
          <p id="sourcesHelp" class="subtle">修改来源后先保存，再到概览同步。所有启用来源通过后一起更新；失败时继续使用原规则，已准备好的来源可复用缓存。</p>
          <SyncProgress v-if="sync?.progress" :progress="sync.progress" :busy="sync.busy" compact />
          <p v-else-if="incomplete" class="sync-note" data-kind="warning">上次有来源未通过，规则未更新。先处理“需要处理”的来源，再次同步即可。</p>
          <div class="source-filter" role="group" aria-label="筛选规则来源"><button type="button" :aria-pressed="filter === 'all'" @click="filter = 'all'">全部 {{ sources.rows.length }}</button><button type="button" :aria-pressed="filter === 'enabled'" @click="filter = 'enabled'">已启用 {{ sources.enabledCount }}</button><button type="button" :aria-pressed="filter === 'issues'" @click="filter = 'issues'">需要处理 {{ attentionCount }}</button></div>
          <div id="sourceList" role="list" aria-describedby="sourcesHelp">
            <SourceCard v-for="(source, index) in rows" :key="source.id" :source="source" :index="index" :count="rows.length" :eager="sourceKeyboard" :history="sources.history" :saving="sources.saving" :testing="sources.testing" :testing-id="sources.testingId" :test-log-id="sources.testLogId" :test-log="sources.testLog" @toggle="sources.toggle(source, $event)" @edit="edit(source, $event)" @test="sources.test(source)" @delete="remove(source)" @preview="previews.open(source)" />
            <p v-if="sources.ready && sources.rows.length && !rows.length" class="subtle">当前筛选下没有来源。</p>
            <p v-if="sources.ready && !sources.rows.length" class="subtle">暂无来源。新增并保存后才能同步。</p>
          </div>
          <div class="row source-controls"><ActionButton id="addSource" aria-haspopup="dialog" aria-controls="sourceEditorDialog" :disabled="!sources.ready || sources.saving || sources.ordinaryCount >= 64" @click="edit(null, $event.currentTarget)">新增来源</ActionButton><ActionButton id="saveSources" variant="primary" :busy="sources.saving" :disabled="!sources.ready || sources.testing || !sources.dirty" @click="sources.save">{{ sources.saving ? '正在保存…' : '保存来源' }}</ActionButton></div>
          <p id="sourcesStatus" class="sync-note" :data-kind="sources.kind" role="status">{{ sources.message }}</p>
          <ActionButton v-if="sources.testPaused" id="resumeSourceTest" icon="sync" :busy="sources.recoveringTest" @click="sources.resumeTest">继续查询测试</ActionButton>
          <div v-if="sources.orphanTest" class="source-history"><p class="subtle">恢复的来源测试：{{ sources.orphanTest }}（不加入来源配置）</p><pre class="source-result" aria-label="恢复的来源测试日志">{{ sources.testLog }}</pre></div>
          <p class="subtle">普通来源支持 Hosts、域名及模块可识别的 AdGuard 网络规则。HaGeZi Pro − Fake 为不可编辑/删除的固定配方。测试只下载和解析，不替换规则或启动引擎。</p>
        </div>
      </details>
      <details id="customRulesDetails" class="card rule-section">
        <summary>自定义规则</summary>
        <div class="rule-section-body">
          <p class="subtle">每行一条规则，支持域名与 AdGuard 网络过滤语法。白名单会自动添加 @@ 前缀。</p>
          <div class="editor-label"><label for="blacklist">黑名单 · 拦截</label><span id="blackCount">{{ custom.blackCount }} 条</span></div>
          <textarea id="blacklist" v-model="custom.black" spellcheck="false" :disabled="!custom.ready || custom.saving" placeholder="ads.example.com&#10;||tracker.example.org^" />
          <div class="editor-label"><label for="whitelist">白名单 · 放行</label><span id="whiteCount">{{ custom.whiteCount }} 条</span></div>
          <textarea id="whitelist" v-model="custom.white" spellcheck="false" :disabled="!custom.ready || custom.saving" placeholder="safe.example.com" />
          <ActionButton id="saveRules" variant="primary" :busy="custom.saving" :disabled="!custom.ready" @click="custom.save">{{ custom.saving ? '正在保存…' : '保存并热重载' }}</ActionButton>
        </div>
      </details>
    </div>
  </div>
  <SourceEditor v-if="sources.editor" :key="editorKey" :model="sources" :trigger="editorTrigger" />
</template>
