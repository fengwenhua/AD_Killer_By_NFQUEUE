// Injected from module/module.prop by Vite; never maintain a second version.
export const BUILD_INFO = typeof __MODULE_INFO__ === 'undefined'
  ? { id: 'AD_Killer_By_NFQUEUE', name: 'AD_Killer_By_NFQUEUE', author: 'AD_Killer_By_NFQUEUE contributors', version: 'unknown', versionCode: null }
  : __MODULE_INFO__;

export function parseModuleInfo(value) {
  if (value?.module !== 'AD_Killer_By_NFQUEUE' || typeof value.version !== 'string' || !/^\d+\.\d+(?:\.\d+)?$/.test(value.version) ||
      !Number.isSafeInteger(value.versionCode) || value.versionCode < 1) return null;
  return { version: value.version, versionCode: value.versionCode };
}

export function releaseNotes(build = BUILD_INFO) {
  return [
    { version: build.version, current: true, title: '模块在线更新', changes: [
      '支持手动检查 GitHub 正式版，以及可选的预览版。',
      '后台下载、SHA-256 校验、管理器安装与重启提示，重新打开界面可恢复任务。',
      '提供管理器标准 updateJson；CI 通过后生成更新元数据并发布预览包。',
    ] },
  ];
}
