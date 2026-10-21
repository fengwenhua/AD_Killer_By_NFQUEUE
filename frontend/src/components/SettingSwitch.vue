<script setup>
defineProps({ id: String, label: String, help: String, checked: Boolean, disabled: Boolean });
defineEmits(['change']);
</script>

<template>
  <div class="switch">
    <slot name="leading" />
    <div class="switch-copy"><label :for="id">{{ label }}</label><p :id="`${id}-help`" class="subtle">{{ help }}</p></div>
    <input :id="id" type="checkbox" :checked="checked" :disabled="disabled" :aria-describedby="`${id}-help`" @change="$emit('change', $event.target.checked)">
  </div>
</template>
