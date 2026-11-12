export function startupDiagnosis(report) {
  if (report?.status !== 'ok' || report.schema !== 1 || report.read_only !== true ||
      typeof report.authorized !== 'boolean' || !Array.isArray(report.checks) || report.checks.length !== 2 ||
      typeof report.running !== 'boolean' || !/^0x[0-9a-f]{8}$/i.test(report.connmark_mask || '') ||
      new Set(report.checks.map(item => item?.family)).size !== 2 ||
      report.checks.some(item => !['ipv4', 'ipv6'].includes(item?.family) || !['clear', 'conflict', 'unknown'].includes(item.status) || typeof item.chain !== 'string')) {
    throw new Error('自检结果不完整，请升级完整模块包或重新检查授权。');
  }
  if (!report.authorized) return { kind: 'warn', title: '需要管理器授权', advice: '请在 KernelSU / APatch 中重新打开模块并确认 root 授权后自检。' };
  if (report.checks.some(item => item.status === 'unknown')) return { kind: 'warn', title: '防火墙状态未能核实', advice: '读取系统防火墙失败，不能当作没有冲突。请重新自检；仍失败时展开详情反馈。' };
  const conflicts = report.checks.filter(item => item.status === 'conflict');
  if (conflicts.length) {
    const xiaomi = conflicts.some(item => item.chain.startsWith('tc_limiter'));
    return { kind: 'bad', title: xiaomi ? '系统 WLAN 流控标记冲突' : '连接标记与其他网络功能冲突',
      advice: '本模块已拒绝接管，以免干扰现有网络。可暂时关闭对应的网络限速或加速功能后再启动，也可将下方自检详情反馈维护者；不要清空系统防火墙。' };
  }
  const tips = {
    rules_empty: '请先点击“同步开源规则”，成功后再启动守护。',
    rule_load_failed: '规则文件读取失败，请先同步开源规则修复后再启动。',
    queue_conflict: '其他程序占用了网络队列，请先暂停对应的网络工具后再启动。',
    cgroup_isolation_failed: '系统后台冻结隔离未通过，请检查管理器授权，并将下方自检详情反馈维护者。',
    recovery_stop_timeout: '旧守护尚未退出，请先暂停并确认停止成功后再启动。',
  };
  const code = String(report.last_error_code || '');
  if (tips[code]) return { kind: 'warn', title: '已定位上次启动问题', advice: tips[code] };
  if (code.startsWith('cleanup_')) return { kind: 'warn', title: '停止后的清理尚未核实', advice: '请先重试“暂停守护”并确认清理成功，再启动。自检不会删除系统规则。' };
  return { kind: 'ok', title: '连接标记检查通过', advice: report.running
    ? '当前没有发现连接标记冲突。进程存在不代表路由已生效，请以概览的完整状态核实结果为准。'
    : '当前没有发现连接标记冲突。可点击“启动守护”重试；若仍失败，展开详情查看历史错误。自检不会自动启动。' };
}
