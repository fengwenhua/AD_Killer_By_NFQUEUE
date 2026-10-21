<script setup>
import { onBeforeUnmount, onMounted, onUnmounted, ref } from 'vue';
import ActionButton from './ActionButton.vue';
const props = defineProps({ model: Object, trigger: Object });
const dialog = ref(null), input = ref(null);
const draft = props.model.editor, origin = props.trigger;
let outside = false, pane, scrollTop = 0;
const viewport = window.visualViewport;
function cancel() {
  // A queued close event from an old instance must not discard a reopened form.
  if (!props.model.saving && props.model.editor === draft) props.model.editor = null;
}
function submit() {
  if (!props.model.applyEdit()) input.value?.focus({ preventScroll: true });
}
function trap(event) {
  if (event.ctrlKey || event.altKey || event.metaKey) return;
  const targets = [...dialog.value.querySelectorAll('button, input')].filter(element => !element.disabled);
  const first = targets[0], last = targets.at(-1), current = document.activeElement;
  if ((!event.shiftKey && current === last) || (event.shiftKey && current === first) || !dialog.value.contains(current)) {
    event.preventDefault(); (event.shiftKey ? last : first)?.focus({ preventScroll: true });
  }
}
function isOutside(event) {
  const rect = dialog.value.getBoundingClientRect();
  return event.target === dialog.value && (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom);
}
function layout() {
  const element = dialog.value;
  const height = viewport?.height || innerHeight;
  element.style.setProperty('--editor-height', `${height}px`);
  element.style.setProperty('--editor-top', `${viewport?.offsetTop || 0}px`);
  element.dataset.compact = String(height < 440);
  // Android's keyboard can shrink only the visual viewport, not 100dvh.
  const focused = document.activeElement;
  if (!element.open || !element.contains(focused)) return;
  const bounds = element.getBoundingClientRect(), field = focused.getBoundingClientRect();
  if (field.bottom > bounds.bottom - 12) element.scrollTop += field.bottom - bounds.bottom + 12;
  else if (field.top < bounds.top + 12) element.scrollTop -= bounds.top + 12 - field.top;
}
onMounted(() => {
  pane = origin?.closest('.page') || document.getElementById('rules');
  scrollTop = pane?.scrollTop || 0;
  layout(); dialog.value.showModal(); input.value.focus({ preventScroll: true });
  viewport?.addEventListener('resize', layout);
  viewport?.addEventListener('scroll', layout);
  window.addEventListener('resize', layout, { passive: true });
});
onBeforeUnmount(() => {
  viewport?.removeEventListener('resize', layout);
  viewport?.removeEventListener('scroll', layout);
  window.removeEventListener('resize', layout);
  dialog.value?.close();
});
onUnmounted(() => {
  if (props.model.editor || pane?.inert) return;
  const target = origin?.isConnected ? origin : document.getElementById('addSource');
  if (target && !target.disabled && !target.closest('[inert]')) target.focus({ preventScroll: true });
  if (pane?.isConnected) pane.scrollTop = scrollTop;
});
</script>

<template>
  <Teleport to="body">
    <dialog id="sourceEditorDialog" ref="dialog" class="source-editor-dialog" aria-labelledby="sourceEditorTitle" aria-describedby="sourceEditorHelp" @cancel.prevent="cancel" @close="cancel" @keydown.tab="trap" @pointerdown="outside = isOutside($event)" @pointerup="outside && isOutside($event) && cancel()">
      <form id="sourceEditor" novalidate @submit.prevent="submit">
        <header class="source-editor-heading"><h2 id="sourceEditorTitle">{{ draft.id === null ? '新增来源' : '编辑来源' }}</h2><ActionButton variant="ghost" :disabled="model.saving" aria-label="关闭来源编辑" @click="cancel">关闭</ActionButton></header>
        <div class="source-editor-body">
          <label for="sourceUrl">来源地址</label>
          <input id="sourceUrl" ref="input" v-model="draft.url" class="search" type="url" inputmode="url" autocomplete="off" autocapitalize="none" spellcheck="false" required autofocus placeholder="https://example.org/rules.txt" :aria-invalid="!!model.editorError" aria-describedby="sourceEditorHelp sourceFormStatus" :disabled="model.saving" @input="model.editorError = ''">
          <p id="sourceEditorHelp" class="subtle"><span class="editor-help-full">支持 HTTP(S) 地址。确认后仅更新待保存列表，仍需点击“保存来源”，再到概览同步规则。</span><span class="editor-help-compact">确认后仍需点击“保存来源”。</span></p>
          <p id="sourceFormStatus" class="sync-note" :data-kind="model.editorError ? 'error' : undefined" role="status">{{ model.editorError }}</p>
        </div>
        <footer class="source-editor-footer"><ActionButton :disabled="model.saving" @click="cancel">取消</ActionButton><button class="btn primary" type="submit" :disabled="model.saving">确认地址</button></footer>
      </form>
    </dialog>
  </Teleport>
</template>
