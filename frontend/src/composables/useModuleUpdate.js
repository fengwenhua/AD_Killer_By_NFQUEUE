import { computed, ref } from 'vue';
import { selectRelease, latestReleaseNotes, compareVersions, versionParts, upgradeAllowed } from '../domain/updates.js';
import { lastJson, validJob as validJobId } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';
import { getStorage } from '../services/storage.js';
import { afterPaint } from '../services/paint.js';

const KEY = 'ad-killer-by-nfqueue-module-update';
const AUTO_KEY = 'ad-killer-by-nfqueue-auto-update-check';
const PREVIEW_KEY = 'ad-killer-by-nfqueue-update-preview';
export function useModuleUpdate(api, info, feedback, storage = getStorage(), pollMs = 800, paint = afterPaint) {
  const saved = key => { try { return storage?.getItem(key) === 'true'; } catch { return false; } };
  const save = (key, value) => { try { storage?.setItem(key, String(value)); } catch { /* session-only preference */ } };
  const life = useLifetime(), preview = ref(saved(PREVIEW_KEY)), autoCheck = ref(saved(AUTO_KEY));
  const checking = ref(false), busy = ref(false);
  const notesLoading = ref(false), latestRelease = ref(null), notesError = ref('');
  const release = ref(null), ready = ref(''), readyChannel = ref('unknown'), reboot = ref(false), confirming = ref(false);
  const message = ref('手动检查 GitHub 正式版；可选择包含预览版。'), kind = ref('');
  let initialized = false, channelRevision = 0;
  const supported = computed(() => info.verified.value && !info.mismatch.value);
  const installedChannel = () => info.releaseChannel?.value || 'unknown';
  const available = computed(() => supported.value && upgradeAllowed(release.value,info.version.value,installedChannel()));
  const canInstall = computed(() => supported.value && !!ready.value && versionParts(ready.value) &&
    readyChannel.value !== 'unknown' &&
    upgradeAllowed({version:ready.value,preview:readyChannel.value==='preview'},info.version.value,installedChannel()) && !reboot.value);
  const status = (text, type = '') => { message.value = text; kind.value = type; };
  const remember = task => { try { task ? storage?.setItem(KEY,JSON.stringify(task)) : storage?.removeItem(KEY); } catch { /* unavailable storage */ } };
  const failure = (result, fallback) => {
    const detail = lastJson(result.stdout)?.message;
    return typeof detail === 'string' && detail.trim() ? detail.slice(0, 500) : fallback;
  };
  async function finishFeedback(started) {
    // Fast failures (e.g. a cached 403) must still have a perceptible busy state.
    if (life.active && typeof requestAnimationFrame === 'function') await life.wait(Math.max(0, 400 - (Date.now() - started)));
  }
  function setPreview(value) {
    preview.value = value === true; save(PREVIEW_KEY, preview.value); channelRevision++;
    release.value = null; confirming.value = false;
    if (!busy.value) status('更新渠道已切换，请重新检查更新。');
  }
  function setAutoCheck(value) {
    autoCheck.value = value === true; save(AUTO_KEY, autoCheck.value);
    if (autoCheck.value && initialized && !reboot.value) void check();
  }
  async function loadReleaseNotes(force = false) {
    if (notesLoading.value || (latestRelease.value && !force)) return;
    const started = Date.now();
    notesLoading.value = true; notesError.value = '';
    try {
      await paint();
      if (!life.active) return;
      const result = await api.command('--module-release-notes');
      if (!life.active) return;
      if (result.code) throw new Error(failure(result, '无法读取 GitHub 发布日志，请检查网络或稍后重试。'));
      const value = latestReleaseNotes(JSON.parse(result.stdout));
      if (!value) throw new Error('GitHub 暂无可用的发布日志。');
      latestRelease.value = value;
    } catch (error) { if (life.active) { latestRelease.value = null; notesError.value = error instanceof SyntaxError ? 'GitHub 发布日志格式无效，请稍后重试。' : error.message; } }
    finally { await finishFeedback(started); notesLoading.value = false; }
  }
  async function state() {
    const result = await api.command('--module-update-status');
    if (result.code) throw new Error(result.stderr || '无法读取模块更新状态');
    const value = lastJson(result.stdout);
    if (!value || typeof value.version !== 'string' || (value.version && !versionParts(value.version))) throw new Error('模块更新状态无效');
    if (value.channel !== undefined && !['stable','preview','unknown'].includes(value.channel)) throw new Error('安装包渠道无效，请重新下载');
    if (life.active) { ready.value = value.version; readyChannel.value = value.channel || 'stable'; reboot.value = value.rebootRequired === true; }
  }
  async function check() {
    if (checking.value || busy.value) return;
    const channel = preview.value, revision = channelRevision;
    const started = Date.now();
    checking.value = true; release.value = null; confirming.value = false;
    status('正在检查 GitHub Release…');
    try {
      await paint();
      if (!life.active || revision !== channelRevision) return;
      const result = await api.command(`--module-update-check ${channel ? 'preview' : 'stable'}`);
      if (!life.active || revision !== channelRevision) return;
      if (result.code) throw new Error(failure(result, '检查失败：网络不可达、GitHub 请求限流或尚无正式版，请稍后重试。'));
      const selected = selectRelease(JSON.parse(result.stdout),channel);
      if (!selected) throw new Error('暂未找到包含有效安装包与校验和的版本。');
      release.value = selected;
      if (!supported.value) status('已获取发布信息；请在匹配的已安装模块界面中下载或安装。','warning');
      else if (available.value) status(`发现 v${selected.version}${selected.preview ? ' 预览版' : ' 正式版'}${compareVersions(selected.version,info.version.value)===0 ? '（预览版升级正式版）' : ''}，可下载并校验。`,'success');
      else if (compareVersions(selected.version,info.version.value)===0 && installedChannel()==='unknown') status('版本号相同，但旧包未记录发布渠道，无法确认是否与正式包一致。请手动覆盖安装正式 ZIP，或等待更高版本。','warning');
      else status(`当前为 v${info.version.value}${installedChannel()==='preview' ? ' 预览版' : installedChannel()==='stable' ? ' 正式版' : ''}，没有可升级版本。`);
    } catch (error) { if (life.active && revision === channelRevision) status(error instanceof SyntaxError ? '更新信息格式无效，请稍后重试。' : error.message,'error'); }
    finally { await finishFeedback(started); checking.value = false; }
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
        if (task.channel && readyChannel.value !== task.channel) throw Object.assign(new Error('下载包渠道与所选发布不一致，请重新下载。'),{terminal:true});
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
      const channel = install ? readyChannel.value : release.value.preview ? 'preview' : 'stable';
      const result = await api.command(`--module-update-${install ? 'install' : 'download'} ${version}${channel==='preview' ? ' preview' : ''}`);
      if (!life.active) return;
      const value = lastJson(result.stdout);
      if (result.code || value?.status !== 'started' || !validJobId(value.job_id)) throw new Error(result.stderr || '无法创建模块更新任务，请检查当前版本或 root 权限。');
      const task = { id: value.job_id, version, install, channel };
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
      else if (ready.value && readyChannel.value==='unknown') status('旧下载缓存未记录渠道，请重新检查并下载后再安装。','warning');
      else if (ready.value) status(`已恢复 v${ready.value}${readyChannel.value==='preview' ? ' 预览版' : ' 正式版'}安装包，SHA-256 校验通过。`,'success');
    } catch (error) { if (life.active) status(error.message,'error'); }
    finally { busy.value = false; }
  }
  async function initialize() {
    if (initialized) return;
    initialized = true;
    await resume();
    if (life.active && autoCheck.value && !reboot.value && kind.value !== 'error') await check();
  }
  return { preview,autoCheck,checking,busy,release,ready,readyChannel,reboot,confirming,message,kind,supported,available,canInstall,
    notesLoading,latestRelease,notesError,loadReleaseNotes,setPreview,setAutoCheck,
    check,download: () => start(false),install: () => start(true),resume,initialize };
}
