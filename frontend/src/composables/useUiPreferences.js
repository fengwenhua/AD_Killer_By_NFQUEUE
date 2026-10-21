import { ref } from 'vue';

export const PREVIEW_DEFAULT = 500, PREVIEW_MIN = 100, PREVIEW_MAX = 5000;
const KEY = 'ad-killer-by-nfqueue-preview-characters';
export function previewCharacters(value) {
  const text = String(value ?? '');
  const count = /^\d+$/.test(text) ? Number(text) : NaN;
  return Number.isInteger(count) && count >= PREVIEW_MIN && count <= PREVIEW_MAX ? count : null;
}

export function useUiPreferences(storage = () => globalThis.localStorage) {
  let saved;
  try {
    const target = storage();
    saved = target?.getItem(KEY);
  } catch { /* restricted WebView storage */ }
  const characters = ref(previewCharacters(saved) ?? PREVIEW_DEFAULT);
  const draft = ref(String(characters.value)), error = ref(''), message = ref('');
  function save() {
    const count = previewCharacters(draft.value);
    error.value = ''; message.value = '';
    if (count === null) { error.value = `请输入 ${PREVIEW_MIN}–${PREVIEW_MAX} 之间的整数`; return false; }
    characters.value = count; draft.value = String(count);
    try {
      const target = storage();
      if (!target) throw new Error('storage unavailable');
      target.setItem(KEY, String(count));
      message.value = '已保存，下次打开内容预览时生效。';
    } catch { message.value = '已在本次打开中生效；当前环境无法持久保存，重新打开后恢复默认。'; }
    return true;
  }
  return { characters, draft, error, message, save };
}
