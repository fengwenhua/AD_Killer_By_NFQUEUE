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
    { version: build.version, current: true, title: '首次开源版本', changes: [
      '项目、模块、二进制、数据目录与发布包统一更名为 AD_Killer_By_NFQUEUE。',
      '完成数据目录、防火墙链、WebUI 与发布包的统一命名。',
      '补充 YH_Hosts 来源致谢、学习用途免责声明与公开发布说明。',
    ] },
  ];
}
