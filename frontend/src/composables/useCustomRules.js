import { computed, ref } from 'vue';
import { PATHS } from '../services/api.js';
import { lines } from '../domain/rules.js';

export function useCustomRules(api, feedback) {
  const black = ref(''), white = ref(''), ready = ref(false), saving = ref(false);
  const blackCount = computed(() => lines(black.value).length), whiteCount = computed(() => lines(white.value).length);
  async function load() {
    ready.value = false;
    try {
      const all = lines(await api.readFile(PATHS.custom));
      black.value = all.filter(s => !s.startsWith('@@')).join('\n');
      white.value = all.filter(s => s.startsWith('@@')).map(s => s.slice(2).trim()).join('\n'); ready.value = true;
    } catch (error) {
      feedback.log('自定义规则未能读取，已禁止保存：' + error.message, 'error');
      feedback.toast('规则读取失败，原文件未修改');
    }
  }
  async function save() {
    if (!ready.value || saving.value) return;
    saving.value = true;
    try {
      const all = [...lines(black.value), ...lines(white.value).map(s => s.startsWith('@@') ? s : '@@' + s)];
      const write = await api.writeFile(PATHS.custom, all.join('\n') + (all.length ? '\n' : ''));
      if (write.code) { feedback.log('自定义规则写入失败：' + write.stderr, 'error'); feedback.toast('保存失败'); return; }
      const result = await api.command('--reload');
      const text = result.code ? '规则已保存，未能提交热重载；启动后生效或查看日志后重试' : '自定义规则已保存，已提交后台热重载；失败原因请查看引擎日志';
      feedback.log(text, result.code ? '' : 'ok'); feedback.toast(text);
    } finally { saving.value = false; }
  }
  return { black, white, ready, saving, blackCount, whiteCount, load, save };
}
