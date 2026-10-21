import { ref, watch } from 'vue';

export function useTheme() {
  let saved;
  try { saved = localStorage.getItem('ad-killer-by-nfqueue-theme'); } catch { /* file origin can deny storage */ }
  const theme = ref(['light', 'dark'].includes(saved) ? saved : matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
  watch(theme, value => {
    document.documentElement.dataset.theme = value;
    try { localStorage.setItem('ad-killer-by-nfqueue-theme', value); } catch { /* no persistence */ }
  }, { immediate: true });
  return { theme, toggle: () => { theme.value = theme.value === 'dark' ? 'light' : 'dark'; } };
}
