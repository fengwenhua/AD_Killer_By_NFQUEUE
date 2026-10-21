import { computed, ref } from 'vue';
import { selectRelease, compareVersions, versionParts } from '../domain/updates.js';
import { lastJson, validJob as validJobId } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';
import { getStorage } from '../services/storage.js';

const KEY = 'ad-killer-by-nfqueue-module-update';
export function useModuleUpdate(api, info, feedback, storage = getStorage(), pollMs = 800) {
  const life = useLifetime(), preview = ref(false), checking = ref(false), busy = ref(false);
  const release = ref(null), ready = ref(''), reboot = ref(false), confirming = ref(false);
  const message = ref('手动检查 GitHub 正式版；可选择包含预览版。'), kind = ref('');
  const supported = computed(() => info.verified.value && !info.mismatch.value);
  const available = computed(() => !!release.value && supported.value && compareVersions(release.value.version,info.version.value) > 0);
  const canInstall = computed(() => supported.value && !!ready.value && versionParts(ready.value) && compareVersions(ready.value,info.version.value)>0 && !reboot.value);
  const status = (text, type = '') => { message.value = text; kind.value = type; };
  const remember = task => { try { task ? storage?.setItem(KEY,JSON.stringify(task)) : storage?.removeItem(KEY); } catch { /* unavailable storage */ } };
  async function state() {
    const result = await api.command('--module-update-status');
    if (result.code) throw new Error(result.stderr || '无法读取模块更新状态');
    const value = lastJson(result.stdout);
    if (!value || typeof value.version !== 'string' || (value.version && !versionParts(value.version))) throw new Error('模块更新状态无效');
    if (life.active) { ready.value = value.version; reboot.value = value.rebootRequired === true; }
  }
  async function check() {
    if (checking.value || busy.value) return;
    checking.value = true; release.value = null; confirming.value = false;
    status('正在检查 GitHub Release…');
    try {
      const result = await api.command(`--module-update-check ${preview.value ? 'preview' : 'stable'}`);
      if (!life.active) return;
      if (result.code) throw new Error('检查失败：GitHub 不可达、请求限流或尚无正式版，请稍后重试或打开发布页。');
      const selected = selectRelease(JSON.parse(result.stdout),preview.value);
      if (!selected) throw new Error('未找到包含有效安装包与校验和的版本，请查看发布页。');
      release.value = selected;
      if (!supported.value) status('已获取发布信息；请在匹配的已安装模块界面中下载或安装。','warning');
      else status(available.value ? `发现 v${selected.version}${selected.preview ? ' 预览版' : ''}，可下载并校验。` : '当前已是最新版本或比发布版本更新。',available.value ? 'success' : '');
    } catch (error) { if (life.active) status(error.message,'error'); }
    finally { checking.value = false; }
  }
  async function observe(task) {
    let errors = 0;
    while (life.active) {
      try {
        const result = await api.command(`--update-status ${task.id}`);
        if (!life.active) return;
        const value = lastJson(result.stdout);
        if (value?.status === 'not_found') {
          remember(null);
          throw Object.assign(new Error('后台任务记录已不存在，请重新下载或查看根管理器中的安装状态。'),{terminal:true});
        }
        if (result.code || !value || !['running','success','failed','interrupted'].includes(value.status)) throw new Error('任务状态暂时不可用');
        errors = 0;
        if (value.status === 'running') { await life.wait(pollMs); continue; }
        remember(null);
        const log = await api.readJob(task.id);
        if (!life.active) return;
        if (log) feedback.log(log,value.status === 'success' ? 'ok' : 'error');
        if (value.status !== 'success' || value.exit_code !== 0) throw Object.assign(new Error('模块更新任务失败或中断，请查看诊断日志后重试。'),{terminal:true});
        await state();
        if (task.install ? !reboot.value : ready.value !== task.version) throw Object.assign(new Error('任务结束，但安装包或安装状态未核实，请重新读取状态。'),{terminal:true});
        status(task.install ? '安装成功，请手动重启设备。重启前继续使用当前版本。' : `v${ready.value} 下载完成，SHA-256 校验通过。`,'success');
        return;
      } catch (error) {
        if (!life.active) return;
        if (error.terminal || ++errors >= 5) {
          status(error.message + (errors >= 5 ? ' 任务可能仍在后台运行，重新打开页面可恢复查询。' : ''),'error');
          return;
        }
        await life.wait(pollMs);
      }
    }
  }
  async function start(install) {
    if (busy.value || checking.value || !supported.value || (install ? !canInstall.value : !available.value)) return;
    const version = install ? ready.value : release.value.version;
    busy.value = true; confirming.value = false;
    status(install ? '正在安装模块，请等待管理器返回结果…' : '正在后台下载并校验安装包…');
    try {
      const result = await api.command(`--module-update-${install ? 'install' : 'download'} ${version}`);
      if (!life.active) return;
      const value = lastJson(result.stdout);
      if (result.code || value?.status !== 'started' || !validJobId(value.job_id)) throw new Error(result.stderr || '无法创建模块更新任务，请检查当前版本或 root 权限。');
      const task = { id: value.job_id, version, install };
      remember(task); await observe(task);
    } catch (error) { if (life.active) status(error.message,'error'); }
    finally { busy.value = false; }
  }
  async function resume() {
    if (busy.value || !supported.value) return;
    busy.value = true;
    try {
      await state();
      let task;
      try { task = JSON.parse(storage?.getItem(KEY) || 'null'); } catch { remember(null); }
      if (task && validJobId(task.id) && versionParts(task.version) && typeof task.install === 'boolean') {
        status('正在恢复后台模块更新任务…'); await observe(task);
      } else if (reboot.value) status('模块已安装，等待手动重启设备。','success');
      else if (ready.value) status(`已恢复 v${ready.value} 安装包，SHA-256 校验通过。`,'success');
    } catch (error) { if (life.active) status(error.message,'error'); }
    finally { busy.value = false; }
  }
  return { preview,checking,busy,release,ready,reboot,confirming,message,kind,supported,available,canInstall,
    check,download: () => start(false),install: () => start(true),resume };
}
