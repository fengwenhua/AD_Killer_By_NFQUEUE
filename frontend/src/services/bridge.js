let callbackId = 0;

export function createBridge(host = globalThis.window, timeoutMs = 120000) {
  const manager = () => host?.ksu?.exec ? host.ksu : host?.apatch?.exec ? host.apatch : null;
  async function run(command) {
    const bridge = manager();
    if (!bridge) return { code: -1, stdout: '', stderr: '未检测到 KernelSU / APatch 环境' };
    return new Promise(resolve => {
      const callback = `adKillerNfqueueExec_${Date.now()}_${callbackId++}`;
      let timer;
      const finish = (code, stdout = '', stderr = '') => {
        clearTimeout(timer);
        delete host[callback];
        resolve({ code: Number.isInteger(Number(code)) ? Number(code) : -1, stdout: String(stdout || ''), stderr: String(stderr || '') });
      };
      host[callback] = finish;
      timer = setTimeout(() => finish(-1, '', '管理器接口未返回，操作结果未知，请刷新状态后再试'), timeoutMs);
      try { bridge.exec(command, '{}', callback); }
      catch (error) { finish(-1, '', String(error)); }
    });
  }
  return { run, packageManager: manager };
}

export function shellQuote(value) {
  return "'" + String(value).replace(/'/g, "'\\''") + "'";
}

export function encodeText(value) {
  return btoa(Array.from(new TextEncoder().encode(value), b => String.fromCharCode(b)).join(''));
}

export function decodeText(value) {
  return new TextDecoder().decode(Uint8Array.from(atob(value), c => c.charCodeAt(0)));
}
