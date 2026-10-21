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
  async function readFile(path) {
    const result = await bridge.run(`base64 ${shellQuote(path)} 2>/dev/null | tr -d '\\n\\r'`);
    if (result.code || !result.stdout.trim()) return '';
    try { return decodeText(result.stdout.trim()); } catch { return ''; }
  }
  const writeFile = (path, text) => bridge.run(`printf '%s' '${encodeText(text)}' | base64 -d > ${shellQuote(path)}`);
  return {
    command, readFile, writeFile,
    saveSources: text => bridge.run(`printf '%s' '${encodeText(text)}' | base64 -d | ${BIN} --sources-save`),
    readJob: id => readFile(`${PATHS.jobs}/update-${id}.log`),
    readDns: () => bridge.run(`tail -n 200 ${shellQuote(PATHS.dns)} 2>/dev/null`),
    packageManager: bridge.packageManager,
  };
}
