<script setup>
import { computed } from 'vue';
const props = defineProps({ progress: Object, busy: Boolean, compact: Boolean });
const rows = computed(() => [...(props.progress?.rows || [])].sort((a, b) =>
  Number(b.display.needsAction) - Number(a.display.needsAction) || Number(b.display.pending) - Number(a.display.pending)));
</script>

<template>
  <div v-if="progress" class="sync-progress" :data-kind="progress.kind">
    <div class="sync-progress-heading"><b role="status">{{ progress.title }}</b><span>{{ progress.done }} / {{ progress.total }}</span></div>
    <progress :value="progress.done" :max="progress.total" aria-label="来源处理进度" />
    <div class="sync-counts"><span>已就绪 <b>{{ progress.ready }}</b></span><span>缓存复用 <b>{{ progress.cached }}</b></span><span :data-failed="progress.needsRetry > 0 || undefined">需重试 <b>{{ progress.needsRetry }}</b></span><span>未完成 <b>{{ progress.total - progress.done }}</b></span></div>
    <p v-if="progress.phase === 'incomplete_sources'" class="subtle">已校验的来源可在有效期内复用缓存。再次同步将重试失败来源，全部通过后一起更新规则。</p>
    <details v-if="!compact" class="sync-source-results" :open="busy || progress.failed > 0">
      <summary>查看 {{ progress.total }} 个来源的进度与结果</summary>
      <ul>
        <li v-for="row in rows" :key="row.url" :data-source="row.url">
          <div class="sync-source-heading"><b :title="row.url">{{ row.name }}</b><span :data-kind="row.display.kind">{{ row.display.label }}</span></div>
          <p v-if="row.display.detail" class="subtle">{{ row.display.detail }}</p>
        </li>
      </ul>
    </details>
  </div>
  <p v-else-if="busy" class="sync-note" role="status">正在获取各来源的处理进度…</p>
</template>
