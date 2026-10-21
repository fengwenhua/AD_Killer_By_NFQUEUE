import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from 'vue';

// Bound DOM/layout work while preserving native scrolling and checkbox keys.
export function useVirtualApps(items, element) {
  const rowHeight = 80, top = ref(0), height = ref(520), columns = ref(1);
  let observer;
  const rows = computed(() => Math.ceil(items.value.length / columns.value));
  const windowRows = computed(() => Math.ceil(height.value / rowHeight) + 7);
  const startRow = computed(() => Math.min(Math.max(0, rows.value - windowRows.value), Math.max(0, Math.floor(top.value / rowHeight) - 3)));
  const endRow = computed(() => Math.min(rows.value, startRow.value + windowRows.value));
  const start = computed(() => startRow.value * columns.value);
  const visible = computed(() => items.value.slice(start.value, endRow.value * columns.value));
  const before = computed(() => startRow.value * rowHeight);
  const after = computed(() => Math.max(0, Math.ceil(items.value.length / columns.value) - endRow.value) * rowHeight);
  const scroll = () => { top.value = element.value.scrollTop; };
  function measure() {
    if (!element.value) return;
    columns.value = getComputedStyle(element.value).gridTemplateColumns.split(' ').length;
    height.value = element.value.clientHeight || 520; scroll();
  }
  watch(items, () => { if (element.value) element.value.scrollTop = 0; top.value = 0; });
  async function keydown(event) {
    if (event.key !== 'Tab' || event.target.type !== 'checkbox') return;
    const index = Number(event.target.dataset.appIndex), target = index + (event.shiftKey ? -1 : 1);
    if (target < 0 || target >= items.value.length) return;
    event.preventDefault();
    const rowTop = Math.floor(target / columns.value) * rowHeight, list = element.value;
    if (rowTop < list.scrollTop) list.scrollTop = rowTop;
    else if (rowTop + rowHeight > list.scrollTop + list.clientHeight) list.scrollTop = rowTop + rowHeight - list.clientHeight;
    scroll(); await nextTick();
    list.querySelector(`[data-app-index="${target}"]`)?.focus({ preventScroll: true });
  }
  onMounted(() => { observer = new ResizeObserver(measure); observer.observe(element.value); measure(); });
  onBeforeUnmount(() => observer?.disconnect());
  return { rowHeight, visible, start, before, after, scroll, keydown };
}
