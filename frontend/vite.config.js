import { defineConfig } from 'vite';
import vue from '@vitejs/plugin-vue';
import { viteSingleFile } from 'vite-plugin-singlefile';
import { readFileSync } from 'node:fs';

const properties = Object.fromEntries(readFileSync(new URL('../module/module.prop', import.meta.url), 'utf8')
  .split(/\r?\n/).filter(line => line.includes('=')).map(line => [line.slice(0, line.indexOf('=')), line.slice(line.indexOf('=') + 1)]));
const moduleInfo = { id: properties.id, name: properties.name, author: properties.author,
  version: properties.version.replace(/^v/, ''), versionCode: Number(properties.versionCode) };
if (!/^\d+\.\d+(?:\.\d+)?$/.test(moduleInfo.version) || !Number.isSafeInteger(moduleInfo.versionCode) || moduleInfo.versionCode < 1) throw new Error('Invalid module.prop version');
const gsapPackage = JSON.parse(readFileSync(new URL('./node_modules/gsap/package.json', import.meta.url), 'utf8'));
const gsapNotice = readFileSync(new URL('./node_modules/gsap/gsap-core.js', import.meta.url), 'utf8').match(/\/\*![\s\S]*?\*\//)?.[0];
if (!gsapNotice) throw new Error('Missing GSAP copyright notice');
const runtimeLicenses = `<!-- Bundled runtime: Vue (MIT)\n${readFileSync(new URL('./node_modules/vue/LICENSE', import.meta.url), 'utf8')}-->\n` +
  `<!-- Bundled runtime: GSAP (Standard License), ${gsapPackage.version}\n${gsapNotice}\n${gsapPackage.license}\nhttps://gsap.com/community/standard-license/\n-->\n`;

// KernelSU/APatch can load webroot via file://. Ship all JS/CSS inline: no
// remote CDN, fetch of local assets, router history, or file-origin CORS.
export default defineConfig({
  base: './',
  define: { __MODULE_INFO__: JSON.stringify(moduleInfo) },
  plugins: [vue(), viteSingleFile(), {
    name: 'bundled-runtime-licenses',
    transformIndexHtml: {
      order: 'post',
      handler: html => html.replace('</head>', runtimeLicenses + '</head>'),
    },
  }],
  build: {
    outDir: '../module/webroot',
    emptyOutDir: true,
    target: 'chrome100',
    sourcemap: false,
    modulePreload: false,
  },
});
