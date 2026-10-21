<script setup>
import { nextTick, ref, watch } from 'vue';
import ActionButton from './ActionButton.vue';
const props = defineProps({ feedback: Object });
const output = ref(null);
watch(() => props.feedback.logs.length && props.feedback.logs.at(-1)?.id, async () => {
  await nextTick(); if (output.value) output.value.scrollTop = output.value.scrollHeight;
});
</script>

<template>
  <details id="consoleDetails" class="card console-card" :open="feedback.consoleOpen" @toggle="feedback.consoleOpen = $event.target.open">
    <summary>运行日志与诊断</summary>
    <div class="console-body">
      <div id="console" ref="output" role="log" aria-label="诊断日志" aria-live="polite"><div v-for="line in feedback.logs" :key="line.id" :class="`log-${line.kind}`">{{ line.text }}</div></div>
      <div class="row"><ActionButton variant="ghost" @click="feedback.clear">清空控制台</ActionButton></div>
    </div>
  </details>
</template>
