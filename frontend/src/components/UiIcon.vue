<script setup>
defineProps({ name: { type: String, default: 'shield' } });
const paths = {
  shield: 'M12 3 4 6v5c0 5 3.5 8 8 10 4.5-2 8-5 8-10V6l-8-3Z M8 12l3 3 5-6',
  'shield-idle': 'M12 3 4 6v5c0 5 3.5 8 8 10 4.5-2 8-5 8-10V6l-8-3Z',
  home: 'm3 10 9-7 9 7 M5 9v12h14V9 M9 21v-7h6v7',
  apps: 'M3 3h7v7H3z M14 3h7v7h-7z M3 14h7v7H3z M14 14h7v7h-7z',
  rules: 'M8 5h13 M8 12h13 M8 19h13 M3 5h.01 M3 12h.01 M3 19h.01',
  theme: 'M12 3v2 M12 19v2 M3 12h2 M19 12h2 M5.6 5.6 7 7 M17 17l1.4 1.4 M5.6 18.4 7 17 M17 7l1.4-1.4 M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0',
  play: 'm8 5 11 7-11 7V5Z',
  pause: 'M8 5v14 M16 5v14',
  sync: 'M20 7v5h-5 M4 17v-5h5 M6 6a8 8 0 0 1 13 2 M18 18a8 8 0 0 1-13-2',
  settings: 'M9 3h6l1 3 3 1 2 5-2 5-3 1-1 3H9l-1-3-3-1-2-5 2-5 3-1 1-3Z M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0',
  sun: 'M12 3v2 M12 19v2 M3 12h2 M19 12h2 M5.6 5.6 7 7 M17 17l1.4 1.4 M5.6 18.4 7 17 M17 7l1.4-1.4 M16 12a4 4 0 1 1-8 0 4 4 0 0 1 8 0',
  moon: 'M20.8 13.1A9 9 0 0 1 10.9 3.2a9 9 0 1 0 9.9 9.9Z',
  info: 'M12 11v6 M12 7h.01 M21 12a9 9 0 1 1-18 0 9 9 0 0 1 18 0',
  download: 'M12 3v12 m-5-5 5 5 5-5 M4 16v5h16v-5',
  external: 'M14 3h7v7 m0-7-11 11 M10 3H3v18h18v-7',
  github: 'M9 19c-4.3 1.3-4.3-2.2-6-2.7 M9 22v-3.9a3.4 3.4 0 0 1 1-2.6C6.7 15.1 3.3 13.9 3.3 8a5.8 5.8 0 0 1 1.6-4A5.4 5.4 0 0 1 5 1s1.3-.4 4 1.5a13.7 13.7 0 0 1 6 0C17.7.6 19 1 19 1a5.4 5.4 0 0 1 .1 3 5.8 5.8 0 0 1 1.6 4c0 5.9-3.4 7.1-6.7 7.5a3.4 3.4 0 0 1 1 2.6V22',
  back: 'm14 6-6 6 6 6',
  chevron: 'm9 6 6 6-6 6',
};
</script>

<template>
  <svg class="icon" viewBox="0 0 24 24" aria-hidden="true"><path :d="paths[name] || paths.shield" /></svg>
</template>
