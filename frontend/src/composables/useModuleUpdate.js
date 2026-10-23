import { computed, ref } from 'vue';
import { selectRelease, latestReleaseNotes, compareVersions, versionParts, upgradeAllowed } from '../domain/updates.js';
import { lastJson, validJob as validJobId } from '../domain/rules.js';
import { useLifetime } from './useLifetime.js';
import { getStorage } from '../services/storage.js';
import { afterPaint } from '../services/paint.js';
import { NOTES_CACHE_KEY, CHECK_CACHE_KEY, NOTES_CACHE_TTL, CHECK_CACHE_TTL, readCache, writeCache, dropCache } from '../services/updateCache.js';
import { discoverJobs, observeJob } from '../domain/jobs.js';

const KEY = 'ad-killer-by-nfqueue-module-update';
const AUTO_KEY = 'ad-killer-by-nfqueue-auto-update-check';
const PREVIEW_KEY = 'ad-killer-by-nfqueue-update-preview';
export function useModuleUpdate(api, info, feedback, storage = getStorage(), pollMs = 800, paint = afterPaint) {
  const saved = key => { try { return storage?.getItem(key) === 'true'; } catch { return false; } };
  const save = (key, value) => { try { storage?.setItem(key, String(value)); } catch { /* session-only preference */ } };
  const life = useLifetime(), preview = ref(saved(PREVIEW_KEY)), autoCheck = ref(saved(AUTO_KEY));
  const checking = ref(false), busy = ref(false);
  const observerPaused = ref(false);
  let rememberedTask;
  const notesLoading = ref(false), latestRelease = ref(null), notesError = ref('');
  const notesUpdatedAt = ref(0), notesCached = ref(false), checkedAt = ref(0), checkCached = ref(false);
  const release = ref(null), ready = ref(''), readyChannel = ref('unknown'), reboot = ref(false), confirming = ref(false);
  const message = ref('手动检查 GitHub 正式版；可选择包含预览版。'), kind = ref('');
  let initialized = false, channelRevision = 0;
  const supported = computed(() => info.verified.value && !info.mismatch.value);
  const installedChannel = () => info.releaseChannel?.value || 'unknown';
  const checkKey = channel => `${CHECK_CACHE_KEY}-${channel ? 'preview' : 'stable'}`;
  const checkContext = () => [info.version.value, info.versionCode?.value ?? '', installedChannel()].join('|');
  const available = computed(() => supported.value && upgradeAllowed(release.value,info.version.value,installedChannel()));
  const canInstall = computed(() => supported.value && !observerPaused.value && !!ready.value && versionParts(ready.value) &&
    readyChannel.value !== 'unknown' &&
    upgradeAllowed({version:ready.value,preview:readyChannel.value==='preview'},info.version.value,installedChannel()) && !reboot.value);
  const status = (text, type = '') => { message.value = text; kind.value = type; };
  const cachedNotes = readCache(storage,NOTES_CACHE_KEY,NOTES_CACHE_TTL,latestReleaseNotes);
  if (cachedNotes) { latestRelease.value=cachedNotes.value; notesUpdatedAt.value=cachedNotes.savedAt; notesCached.value=true; }
  function describeRelease(cached = false) {
    if (!release.value) return;
    const selected=release.value, prefix=cached ? '已恢复上次检查：' : '';
    if (!supported.value) status('已获取发布信息；请在匹配的已安装模块界面中下载或安装。','warning');
    else if (available.value) status(`${prefix}发现 v${selected.version}${selected.preview ? ' 预览版' : ' 正式版'}${compareVersions(selected.version,info.version.value)===0 ? '（预览版升级正式版）' : ''}，可下载并校验。`,'success');
    else if (compareVersions(selected.version,info.version.value)===0 && installedChannel()==='unknown') status('版本号相同，但旧包未记录发布渠道，无法确认是否与正式包一致。请手动覆盖安装正式 ZIP，或等待更高版本。','warning');
    else status(`${prefix}当前为 v${info.version.value}${installedChannel()==='preview' ? ' 预览版' : installedChannel()==='stable' ? ' 正式版' : ''}，没有可升级版本。`);
  }
  function restoreCheck() {
    const cached=readCache(storage,checkKey(preview.value),CHECK_CACHE_TTL,response=>selectRelease(response,preview.value),checkContext());
    if (!cached) return;
    release.value=cached.value; checkedAt.value=cached.savedAt; checkCached.value=true;
    describeRelease(true);
  }
  const remember = task => { rememberedTask = task; try { task ? storage?.setItem(KEY,JSON.stringify(task)) : storage?.removeItem(KEY); } catch { /* unavailable storage */ } };
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
    checkedAt.value=0; checkCached.value=false;
    if (!busy.value) status('更新渠道已切换，请重新检查更新。');
    if (!busy.value) restoreCheck();
  }
  function setAutoCheck(value) {
    autoCheck.value = value === true; save(AUTO_KEY, autoCheck.value);
    if (autoCheck.value && initialized && !reboot.value) void check();
  }
  async function loadReleaseNotes(force = false) {
    if (notesLoading.value || (latestRelease.value && !force && !notesError.value && Date.now()-notesUpdatedAt.value<=NOTES_CACHE_TTL)) return;
    const started = Date.now();
    notesLoading.value = true; notesError.value = '';
    try {
      await paint();
      if (!life.active) return;
      const result = await api.command('--module-release-notes');
      if (!life.active) return;
      if (result.code) throw new Error(failure(result, '无法读取 GitHub 发布日志，请检查网络或稍后重试。'));
      const response=JSON.parse(result.stdout), value = latestReleaseNotes(response);
      if (!value) throw new Error('GitHub 暂无可用的发布日志。');
      latestRelease.value = value;
      notesUpdatedAt.value=Date.now(); notesCached.value=false;
      writeCache(storage,NOTES_CACHE_KEY,response);
    } catch (error) { if (life.active) { notesCached.value=!!latestRelease.value; notesError.value = (error instanceof SyntaxError ? 'GitHub 发布日志格式无效，请稍后重试。' : error.message) + (latestRelease.value ? ' 仍显示上次成功读取的记录。' : ''); } }
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
    checking.value = true; release.value = null; confirming.value = false; checkCached.value=false;
    status('正在检查 GitHub Release…');
    try {
      await paint();
      if (!life.active || revision !== channelRevision) return;
      const result = await api.command(`--module-update-check ${channel ? 'preview' : 'stable'}`);
      if (!life.active || revision !== channelRevision) return;
      if (result.code) throw new Error(failure(result, '检查失败：网络不可达、GitHub 请求限流或尚无正式版，请稍后重试。'));
      const response=JSON.parse(result.stdout), selected = selectRelease(response,channel);
      if (!selected) throw new Error('暂未找到包含有效安装包与校验和的版本。');
      release.value = selected;
      checkedAt.value=Date.now(); writeCache(storage,checkKey(channel),response,checkContext());
      describeRelease();
    } catch (error) { if (life.active && revision === channelRevision) { checkedAt.value=0; dropCache(storage,checkKey(channel)); status(error instanceof SyntaxError ? '更新信息格式无效，请稍后重试。' : error.message,'error'); } }
    finally { await finishFeedback(started); checking.value = false; }
  }
  async function observe(task) {
    rememberedTask = task; observerPaused.value = false;
    const outcome = await observeJob(api, task.id, life, {
      interval: pollMs, types: ['download', 'install'],
      onLog: chunk => { if (chunk.delta) feedback.log(chunk.delta); },
      onRetry: () => status('后台任务状态未能核实，正在退避重试；不会重新下载或安装。','warning'),
    });
    if (!life.active || outcome.kind === 'cancelled') return;
    if (outcome.kind === 'unknown') {
      observerPaused.value = true;
      status('任务结果未确认，已暂停自动查询。任务可能仍在后台运行；点击“继续查询后台任务”，不会重新派发。','warning'); return;
    }
    if (outcome.kind === 'unavailable') {
      remember(null); observerPaused.value = true;
      status('后台任务记录已不存在，下载或安装结果未确认；请继续查询后端任务，或核查根管理器状态。','warning'); return;
    }
    remember(null);
    const value = outcome.state;
    if (value.status !== 'success' || value.exit_code !== 0) { status('模块更新任务失败或中断，请查看诊断日志及管理器状态后重试。','error'); return; }
    await state();
    if (!life.active) return;
    if (task.install ? !reboot.value : ready.value !== task.version) throw new Error('任务结束，但安装包或安装状态未核实，请重新读取状态。');
    if (task.channel && readyChannel.value !== task.channel) throw new Error('下载包渠道与所选发布不一致，请重新下载。');
    status((task.install ? '安装成功，请手动重启设备。重启前继续使用当前版本。' : `v${ready.value} 下载完成，SHA-256 校验通过。`) + (outcome.logError ? ' 日志片段未能完整读取。' : ''),'success');
  }
  async function start(install) {
    if (observerPaused.value) return resume();
    if (busy.value || checking.value || !supported.value || (install ? !canInstall.value : !available.value)) return;
    if (!install && checkCached.value) {
      await check();
      if (!life.active || !available.value || kind.value==='error') return;
    }
    const version = install ? ready.value : release.value.version;
    busy.value = true; confirming.value = false;
    status(install ? '正在安装模块，请等待管理器返回结果…' : '正在后台下载并校验安装包…');
    try {
      const found = await discoverJobs(api, life, ['download', 'install']);
      if (!life.active || found.cancelled) return;
      if (found.unknown) { observerPaused.value = true; status('后台任务未能核实，请继续查询；不会重复下载或安装。','warning'); return; }
      if (found.job && ['running','unverified','invalid_state','not_found'].includes(found.job.status)) {
        const task = backendTask(found.job);
        if (!task) { observerPaused.value = true; status('后台任务目标或渠道未能核实，请查看管理器与诊断日志。','warning'); return; }
        remember(task); await observe(task); return;
      }
      const channel = install ? readyChannel.value : release.value.preview ? 'preview' : 'stable';
      const result = await api.command(`--module-update-${install ? 'install' : 'download'} ${version}${channel==='preview' ? ' preview' : ''}`);
      if (!life.active) return;
      const value = lastJson(result.stdout);
      if (result.code || value?.status !== 'started' || !validJobId(value.job_id)) throw new Error(result.stderr || '无法创建模块更新任务，请检查当前版本或 root 权限。');
      const task = { id: value.job_id, version, install, channel };
      remember(task); await observe(task);
    } catch (error) { if (life.active) { observerPaused.value = true; status(error.message + ' 请先继续查询结果，勿重复派发。','warning'); } }
    finally { busy.value = false; }
  }
  function backendTask(job) {
    return validJobId(job.job_id) && versionParts(job.target) && ['stable','preview'].includes(job.channel)
      ? { id: job.job_id, version: job.target, install: job.type === 'install', channel: job.channel } : null;
  }
  async function resume() {
    if (busy.value || !supported.value) return;
    busy.value = true;
    try {
      await state();
      let task;
      try { task = rememberedTask || JSON.parse(storage?.getItem(KEY) || 'null'); } catch { remember(null); }
      const found = await discoverJobs(api, life, ['download', 'install']);
      if (!life.active || found.cancelled) return;
      if (found.job && ['running','unverified','invalid_state','not_found'].includes(found.job.status)) {
        task = backendTask(found.job);
        if (!task) { observerPaused.value = true; status('后台任务目标或渠道未能核实，请查看诊断日志。','warning'); return; }
        remember(task);
      }
      if (task && validJobId(task.id) && versionParts(task.version) && typeof task.install === 'boolean') {
        status('正在恢复后台模块更新任务…'); await observe(task);
      } else if (found.unknown) { observerPaused.value = true; status('后台任务索引未能核实，请继续查询；不会下载或安装。','warning'); }
      else {
        observerPaused.value = false;
        if (reboot.value) status('模块已安装，等待手动重启设备。','success');
        else if (ready.value && readyChannel.value==='unknown') status('旧下载缓存未记录渠道，请重新检查并下载后再安装。','warning');
        else if (ready.value) status(`已恢复 v${ready.value}${readyChannel.value==='preview' ? ' 预览版' : ' 正式版'}安装包，SHA-256 校验通过。`,'success');
      }
    } catch (error) { if (life.active) { observerPaused.value = true; status(error.message,'error'); } }
    finally { busy.value = false; }
  }
  async function initialize() {
    if (initialized) return;
    initialized = true;
    await resume();
    if (!life.active) return;
    if (!reboot.value && !observerPaused.value && kind.value!=='error') restoreCheck();
    if (life.active && !observerPaused.value && autoCheck.value && !reboot.value && kind.value !== 'error') await check();
  }
  return { preview,autoCheck,checking,busy,observerPaused,release,ready,readyChannel,reboot,confirming,message,kind,supported,available,canInstall,
    notesLoading,latestRelease,notesError,notesUpdatedAt,notesCached,checkedAt,checkCached,loadReleaseNotes,setPreview,setAutoCheck,
    check,download: () => start(false),install: () => start(true),resume,initialize };
}
