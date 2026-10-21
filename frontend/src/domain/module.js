// Injected from module/module.prop by Vite; never maintain a second version.
export const BUILD_INFO = typeof __MODULE_INFO__ === 'undefined'
  ? { id: 'AD_Killer_By_NFQUEUE', name: 'AD Killer', author: 'fengwenhua', version: 'unknown', versionCode: null }
  : __MODULE_INFO__;

export function parseModuleInfo(value) {
  if (value?.module !== 'AD_Killer_By_NFQUEUE' || typeof value.version !== 'string' || !/^\d+\.\d+(?:\.\d+)?$/.test(value.version) ||
      !Number.isSafeInteger(value.versionCode) || value.versionCode < 1) return null;
  return { version: value.version, versionCode: value.versionCode };
}
