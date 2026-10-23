import { createBridge, decodeText, encodeText, shellQuote } from './bridge.js';

export const BIN = '/data/adb/modules/AD_Killer_By_NFQUEUE/AD_Killer_By_NFQUEUE';
const DATA = '/data/adb/AD_Killer_By_NFQUEUE';
export const PATHS = Object.freeze({
  custom: `${DATA}/custom_rules.txt`,
  allow: `${DATA}/app_whitelist.txt`,
  intercept: `${DATA}/app_intercept.txt`,
  stats: `${DATA}/rule_stats.conf`,
  sources: `${DATA}/source_stats.conf`,
  jobs: `${DATA}/update_jobs`,
  dns: `${DATA}/log/dns_query.log`,
});

export function createApi(host = globalThis.window) {
  const bridge = createBridge(host);
  const command = args => bridge.run(`${BIN} ${args}`);
  const targets = new Map([[PATHS.custom, 'custom'], [PATHS.allow, 'allow'], [PATHS.intercept, 'intercept']]);
  const revisions = new Map();
  async function readFile(path) {
    const target = targets.get(path);
    if (target) {
      const result = await command(`--config-read ${target}`);
      let value;
      try { value = JSON.parse(result.stdout); } catch { /* invalid/old backend is never a valid empty file */ }
      if (result.code || value?.status !== 'ok' || value.target !== target ||
          typeof value.content !== 'string' || !/^[a-f0-9]{64}$/.test(value.revision || '')) {
        revisions.delete(path);
        throw new Error(value?.message || result.stderr || '配置读取失败或模块不支持安全编辑，请重新加载或升级完整模块包。');
      }
      revisions.set(path, value.revision);
      return value.content;
    }
    const result = await bridge.run(`base64 ${shellQuote(path)} 2>/dev/null | tr -d '\\n\\r'`);
    if (result.code || !result.stdout.trim()) return '';
    try { return decodeText(result.stdout.trim()); } catch { return ''; }
  }
  async function writeFile(path, text) {
    const target = targets.get(path), revision = revisions.get(path);
    if (!target || !revision) return { code: 1, stdout: '', stderr: '必须先成功读取指定配置后再保存，原文件未修改。' };
    const result = await bridge.run(`printf '%s' '${encodeText(text)}' | base64 -d | ${BIN} --config-save ${target} ${revision}`);
    let value;
    try { value = JSON.parse(result.stdout); } catch { /* result remains unknown */ }
    if (!result.code && value?.status === 'ok' && value.target === target && /^[a-f0-9]{64}$/.test(value.revision || '')) {
      revisions.set(path, value.revision);
      return result;
    }
    // A conflict must preserve the original revision: retrying the same draft
    // must not silently acquire the other window's revision and overwrite it.
    return { ...result, code: result.code || 1,
      stderr: value?.message || result.stderr || '保存结果未确认，请重新读取配置并核对。' };
  }
  return {
    command, readFile, writeFile,
    saveSources: text => bridge.run(`printf '%s' '${encodeText(text)}' | base64 -d | ${BIN} --sources-save`),
    readJob: id => readFile(`${PATHS.jobs}/update-${id}.log`),
    readDns: () => bridge.run(`tail -n 200 ${shellQuote(PATHS.dns)} 2>/dev/null`),
    packageManager: bridge.packageManager,
  };
}
