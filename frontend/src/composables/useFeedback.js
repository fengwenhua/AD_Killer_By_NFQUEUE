import { ref, onScopeDispose } from 'vue';

export function useFeedback() {
  const logs = ref([]), consoleOpen = ref(false), toastText = ref('');
  let sequence = 0, toastTimer;
  function log(text, kind = '') {
    for (const line of String(text).split(/\r?\n/).filter(line => line.trim())) logs.value.push({ id: sequence++, text: line, kind });
    if (logs.value.length > 400) logs.value.splice(0, logs.value.length - 400);
  }
  function toast(text) {
    toastText.value = text;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => { toastText.value = ''; }, 3000);
  }
  onScopeDispose(() => clearTimeout(toastTimer));
  return { logs, consoleOpen, toastText, log, toast, clear: () => { logs.value = []; toast('控制台已清空'); } };
}
