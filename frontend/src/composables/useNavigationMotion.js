import { onMounted, onBeforeUnmount, ref, watch } from 'vue';
import { gsap } from 'gsap';

// Only animate viewport-sized surfaces. Native per-tab scrollTop stays intact.
export function useNavigationMotion(page, main, theme, themeButton) {
  const leaving = ref([]), surfaces = new Map();
  const order = ['home', 'apps', 'rules', 'settings'];
  let ready = false, entrance, themeMotion, media, width = 0;
  const reduced = () => media?.matches || document.hidden;
  function navigate(id) {
    if (!order.includes(id) || id === page.value) return;
    const previous = page.value;
    page.value = id; // The button and the interactive page always agree.
    if (!ready || reduced() || !width) { finish(); return; }

    // Retarget at the current presentation, never queue input behind an old
    // animation or snap a moving page to zero. At most four viewport surfaces
    // exist, with a single style read per surface per tap (not per frame).
    const positions = new Map([...surfaces].map(([key, surface]) =>
      [key, new DOMMatrixReadOnly(getComputedStyle(surface.element).transform).m41]));
    entrance = null;
    for (const surface of surfaces.values()) surface.motion?.cancel();
    if (!positions.size) positions.set(previous, 0);
    if (!positions.has(id)) {
      const direction = order.indexOf(id) > order.indexOf(previous) ? 1 : -1;
      const edge = direction > 0 ? Math.max(...positions.values()) : Math.min(...positions.values());
      positions.set(id, edge + width * direction);
    }
    const destination = positions.get(id);
    if (Math.abs(destination) < .5) { finish(); return; }
    leaving.value = [...positions.keys()].filter(key => key !== id);
    const move = x => `translate3d(${x}px, 0, 0)`;
    const options = { duration: 200, easing: 'cubic-bezier(.22, 1, .36, 1)', fill: 'both' };
    const motions = [];
    for (const [key, x] of positions) {
      const element = main.value.querySelector(`#${key}`);
      const motion = element.animate([{ transform: move(x) }, { transform: move(x - destination) }], options);
      surfaces.set(key, { element, motion });
      motions.push(motion);
    }
    const current = {};
    entrance = current;
    void Promise.all(motions.map(motion => motion.finished)).then(() => {
      if (entrance === current) finish();
    }).catch(() => { /* a newer tap, resize, reduced motion or teardown canceled this run */ });
  }
  function finish() {
    entrance = null;
    for (const surface of surfaces.values()) surface.motion?.cancel();
    surfaces.clear(); leaving.value = [];
  }
  watch(theme, () => {
    themeMotion?.revert(); themeMotion = null;
    const icon = themeButton.value?.querySelector('.icon');
    if (!ready || reduced() || !icon) return;
    themeMotion = gsap.context(() => gsap.fromTo(icon, { rotation: -45, scale: .75 },
      { rotation: 0, scale: 1, duration: .25, ease: 'power2.out', clearProps: 'transform' }), themeButton.value);
  }, { flush: 'post' });
  const resize = () => { width = main.value.clientWidth; finish(); };
  const preference = () => { finish(); themeMotion?.revert(); themeMotion = null; };
  const visibility = () => { if (document.hidden) preference(); };
  onMounted(() => {
    ready = true; width = main.value.clientWidth;
    media = window.matchMedia('(prefers-reduced-motion: reduce)');
    media.addEventListener('change', preference);
    window.addEventListener('resize', resize, { passive: true });
    document.addEventListener('visibilitychange', visibility);
  });
  onBeforeUnmount(() => {
    ready = false;
    media?.removeEventListener('change', preference);
    window.removeEventListener('resize', resize); document.removeEventListener('visibilitychange', visibility);
    finish(); themeMotion?.revert();
  });
  return { leaving, selected: page, navigate };
}
