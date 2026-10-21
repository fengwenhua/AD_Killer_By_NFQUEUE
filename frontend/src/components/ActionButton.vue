<script setup>
import UiIcon from './UiIcon.vue';
defineProps({ busy: Boolean, disabled: Boolean, icon: String, variant: { type: String, default: '' } });
</script>

<template>
  <button type="button" class="btn" :class="[variant, { loading: busy }]" :disabled="disabled || busy" :aria-busy="busy">
    <UiIcon v-if="icon" :name="icon" /><slot />
  </button>
</template>
