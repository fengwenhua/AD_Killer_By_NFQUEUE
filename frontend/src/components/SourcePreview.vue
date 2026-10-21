<script setup>
import { nextTick, onBeforeUnmount, ref, watch } from 'vue';
import ActionButton from './ActionButton.vue';
const props = defineProps({ model: Object });
const dialog = ref(null);
let trigger, outside = false;
watch(() => !!props.model.active, async open => {
  await nextTick();
  if (open && props.model.active && !dialog.value.open) {
    trigger = document.activeElement; dialog.value.showModal();
  } else if (!open && dialog.value.open) dialog.value.close();
}, { flush: 'post' });
function closed() {
  if (dialog.value?.open) return; // an older close event must not close a reopened modal
  props.model.close();
  if (trigger?.isConnected && !trigger.closest('[inert]')) trigger.focus({ preventScroll: true });
}
function trap(event) {
  if (event.ctrlKey || event.altKey || event.metaKey) return;
  const targets = [...dialog.value.querySelectorAll('button, a[href], input, textarea, select, [tabindex]')]
    .filter(element => !element.disabled && element.tabIndex >= 0 && element.getClientRects().length);
  const first = targets[0], last = targets.at(-1), current = document.activeElement;
  if ((!event.shiftKey && current === last) || (event.shiftKey && current === first) || !dialog.value.contains(current)) {
    event.preventDefault(); (event.shiftKey ? last : first)?.focus({ preventScroll: true });
  }
}
function isOutside(event) {
  const rect = dialog.value.getBoundingClientRect();
  return event.target === dialog.value && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom);
}
onBeforeUnmount(() => dialog.value?.close());
</script>

<template>
  <Teleport to="body">
    <dialog id="sourcePreviewDialog" ref="dialog" class="preview-dialog" aria-labelledby="previewTitle" aria-describedby="previewHelp" @cancel.prevent="model.close" @close="closed" @keydown.tab="trap" @pointerdown="outside = isOutside($event)" @pointerup="outside && isOutside($event) && model.close()">
      <template v-if="model.active">
        <header class="preview-heading"><div><span class="eyebrow">SOURCE PREVIEW</span><h2 id="previewTitle">来源内容预览</h2></div><ActionButton autofocus aria-label="关闭内容预览" @click="model.close">关闭</ActionButton></header>
        <div class="preview-scroll">
          <p id="previewHelp" class="subtle">只读查看原始来源开头，每份最多 {{ model.active.characters }} 个字符；不修改规则或同步记录。字符数可在设置中调整。</p>
          <p v-if="model.active.builtin" class="subtle">内置来源分别展示 Pro / Fake 原文，不是差集结果。</p>
          <section v-for="(item, index) in model.active.items" :key="item.url + index" class="preview-item" :aria-busy="item.pending">
            <b v-if="item.label">{{ item.label }}</b><span class="source-url">{{ item.url }}</span>
            <p v-if="item.pending" class="preview-loading" role="status">正在读取来源开头…<span class="subtle">首次需等待网络；成功内容缓存 5 分钟。</span></p>
            <p v-else-if="item.error" class="sync-note" data-kind="error" role="status">{{ item.error }}</p>
            <template v-else><pre class="source-result" tabindex="0">{{ item.text || '（来源内容为空）' }}</pre><p class="subtle">{{ Array.from(item.text).length }} 字符{{ item.truncated ? ' · 后续内容已省略' : ' · 已到结尾' }}</p><time v-if="item.at" class="source-url" :datetime="new Date(item.at).toISOString()">读取于 {{ new Date(item.at).toLocaleString('zh-CN', { hour12: false }) }}</time></template>
          </section>
        </div>
        <footer class="preview-footer"><span class="subtle">预览不代表来源测试通过。</span><ActionButton :disabled="model.pending" @click="model.refresh">重新获取</ActionButton></footer>
      </template>
    </dialog>
  </Teleport>
</template>
