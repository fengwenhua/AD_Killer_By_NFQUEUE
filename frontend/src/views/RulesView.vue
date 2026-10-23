<script setup>
import { nextTick, onBeforeUnmount, onMounted, ref } from 'vue';
import SourceCard from '../components/SourceCard.vue';
import ActionButton from '../components/ActionButton.vue';
import SourceEditor from '../components/SourceEditor.vue';
const props = defineProps({ sources: Object, custom: Object, previews: Object });
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
          <p id="sourcesHelp" class="subtle">支持新增、编辑、启停和单源测试。修改后点击“保存来源”，再到概览同步；停用不会立即删除已加载规则。开关切换不排序，保存成功后未启用项才移到底部。</p>
          <div id="sourceList" role="list" aria-describedby="sourcesHelp">
            <SourceCard v-for="(source, index) in sources.displayRows" :key="source.id" :source="source" :index="index" :count="sources.displayRows.length" :eager="sourceKeyboard" :history="sources.history" :saving="sources.saving" :testing="sources.testing" :testing-id="sources.testingId" :test-log-id="sources.testLogId" :test-log="sources.testLog" @toggle="sources.toggle(source, $event)" @edit="edit(source, $event)" @test="sources.test(source)" @delete="remove(source)" @preview="previews.open(source)" />
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
