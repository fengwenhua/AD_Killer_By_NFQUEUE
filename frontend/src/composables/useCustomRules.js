import { computed, ref } from 'vue';
import { PATHS } from '../services/api.js';
import { lastJson, lines } from '../domain/rules.js';
import { observeApply, validApplyRevision } from '../domain/apply.js';
import { useLifetime } from './useLifetime.js';

export function useCustomRules(api, feedback) {
  const life = useLifetime();
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
      await life.wait(32);
      if (!life.active) return;
      const all = [...lines(black.value), ...lines(white.value).map(s => s.startsWith('@@') ? s : '@@' + s)];
      const write = await api.writeFile(PATHS.custom, all.join('\n') + (all.length ? '\n' : ''));
      if (!life.active) return;
      if (write.code) { feedback.log('自定义规则写入失败：' + write.stderr, 'error'); feedback.toast('保存失败'); return; }
      const revision = lastJson(write.stdout)?.revision;
      const result = await api.command('--reload' + (validApplyRevision(revision) ? ' custom ' + revision : ''));
      const outcome = await observeApply(api, result, life);
      if (!life.active) return;
      feedback.log(outcome.message, outcome.level); feedback.toast(outcome.message);
    } catch (error) {
      if (life.active) { feedback.log('保存或应用结果未确认：' + error.message, 'error'); feedback.toast('结果未确认，请重新读取配置并核对'); }
    } finally { saving.value = false; }
  }
  return { black, white, ready, saving, blackCount, whiteCount, load, save };
}
