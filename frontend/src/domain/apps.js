// Counts come from a verified, complete PackageManager query for this save.
export function applicationNotes(request, phase) {
  if (!['applied', 'deferred'].includes(phase)) return [];
  const count = value => Number.isSafeInteger(value) && value > 0 && value <= 100000 ? value : 0;
  const missing = count(request?.uninstalled_count), shared = count(request?.shared_package_count);
  return [
    missing ? `${missing} 个所选应用当前未安装，已保留选择，安装后会自动识别。` : '',
    shared ? `${shared} 个其他应用与所选应用共用系统编号，将一同受到名单影响。` : '',
  ].filter(Boolean);
}
