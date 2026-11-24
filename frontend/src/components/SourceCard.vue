<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue';
import { sourceHistory } from '../domain/rules.js';
import { sourceName } from '../domain/syncProgress.js';
import ActionButton from './ActionButton.vue';
import { copyText } from '../services/clipboard.js';
const props = defineProps({ source: Object, index: Number, count: Number, eager: Boolean, history: Array, saving: Boolean, testing: Boolean, testingId: [Number, String], testLogId: [Number, String], testLog: String });
defineEmits(['toggle', 'edit', 'test', 'delete', 'preview']);
const record = computed(() => sourceHistory(props.source, props.history));
const busy = computed(() => props.saving || props.testingId === props.source.id);
const title = computed(() => sourceName(props.source.url, props.source.name));
const addressOpen = ref(false), copyStatus = ref('');
async function copyAddress() {
  copyStatus.value = await copyText(props.source.url) ? '地址已复制' : '复制未成功，请长按完整地址复制';
}
watch(() => props.source.url, () => { copyStatus.value = ''; addressOpen.value = false; });
const card = ref(null), body = ref(null), nearby = ref(false), focused = ref(false), height = ref(280);
const visible = computed(() => props.eager || props.index < 8 || nearby.value || focused.value);
let intersection, resize;
function measure() {
  const value = card.value?.getBoundingClientRect().height;
  if (value > 0) height.value = value;
}
// Freeze the exact fractional height BEFORE detaching, even when a fast
// keyboard/pointer switch happens before ResizeObserver's first delivery.
watch(visible, value => { if (!value && body.value) measure(); }, { flush: 'pre' });
watch(body, element => { resize?.disconnect(); if (element) resize?.observe(element); }, { flush: 'post' });
onMounted(() => {
  resize = new ResizeObserver(() => { if (visible.value) measure(); });
  if (body.value) resize.observe(body.value);
  intersection = new IntersectionObserver(entries => { nearby.value = entries[0].isIntersecting; },
    { root: document.getElementById('rules'), rootMargin: '800px 0px' });
  intersection.observe(card.value);
});
onBeforeUnmount(() => { intersection?.disconnect(); resize?.disconnect(); });
</script>

<template>
  <article ref="card" class="source-item" role="listitem" :aria-posinset="index + 1" :aria-setsize="count" :data-builtin="source.locked" :data-source-url="source.url" :style="visible ? undefined : { minHeight: height + 'px' }" @focusin="focused = true" @focusout="focused = !!$event.relatedTarget && card.contains($event.relatedTarget)">
    <div v-if="visible" ref="body" class="source-card-content">
    <div class="source-top">
      <label :for="`source-${source.id}`">{{ title }}</label>
      <input :id="`source-${source.id}`" type="checkbox" :aria-label="`启用${title}`" :checked="source.enabled" :disabled="busy" @change="$emit('toggle', $event.target.checked)">
    </div>
    <details v-if="!source.locked" class="source-address" :open="addressOpen" @toggle="addressOpen = $event.target.open">
      <summary :aria-label="`${addressOpen ? '收起' : '展开'}${title}的完整地址`"><span class="source-url">{{ source.url }}</span><span class="address-hint">{{ addressOpen ? '收起地址' : '展开地址' }}</span></summary>
      <div class="source-address-full"><code tabindex="0" aria-label="完整来源地址">{{ source.url }}</code><ActionButton variant="ghost" @click="copyAddress">复制地址</ActionButton><span class="subtle" role="status">{{ copyStatus }}</span></div>
    </details>
    <p v-else class="source-url source-builtin-hint">内置特殊来源 · 固定配方</p>
    <details v-if="source.locked" class="source-extra"><summary>规则说明与地址</summary>
      <p :id="`source-lock-${source.id}`" class="subtle">固定下载 Pro 和 Fake，组内去重后取差集；不可编辑或删除，可测试、启停。保存并同步后生效。</p>
      <span v-for="(url, n) in source.dependencies" :key="url" class="source-url">{{ n === 0 ? 'Pro' : 'Fake' }}：{{ url }}</span>
    </details>
    <div class="source-history">
      <div class="source-sync-row"><span class="source-sync-state" :data-kind="record.kind">{{ record.label }}</span><time v-if="record.datetime" :datetime="record.datetime">{{ record.time }}</time><span v-else-if="record.time">{{ record.time }}</span></div>
      <span>{{ record.detail }}</span>
    </div>
    <details v-if="record.downloads?.length" class="source-extra source-downloads"><summary>{{ record.label.includes('缓存') ? '缓存内容的下载地址' : '实际下载地址' }}</summary>
      <span v-for="url in record.downloads" :key="url" class="source-url">{{ url }}</span>
    </details>
    <details v-if="record.technical" class="source-extra"><summary>查看详细原因</summary><pre>{{ record.technical }}</pre></details>
    <pre v-if="testLogId === source.id" :id="`sourceTestLog-${source.id}`" class="source-result" aria-label="来源测试日志">{{ testLog }}</pre>
    <div class="source-actions">
      <ActionButton variant="ghost" aria-haspopup="dialog" aria-controls="sourceEditorDialog" :disabled="busy || source.locked" :aria-describedby="source.locked ? `source-lock-${source.id}` : undefined" @click="$emit('edit', $event.currentTarget)">编辑</ActionButton>
      <ActionButton variant="ghost preview-button" aria-haspopup="dialog" aria-controls="sourcePreviewDialog" :disabled="saving" @click="$emit('preview')">查看内容</ActionButton>
      <ActionButton variant="ghost" :disabled="saving || testing" @click="$emit('test')">测试</ActionButton>
      <ActionButton variant="ghost" :disabled="busy || source.locked" :aria-describedby="source.locked ? `source-lock-${source.id}` : undefined" @click="$emit('delete')">删除</ActionButton>
    </div>
    </div>
  </article>
</template>
