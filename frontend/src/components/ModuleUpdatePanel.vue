<script setup>
import { computed, nextTick, onBeforeUnmount, ref, watch } from 'vue';
import ActionButton from './ActionButton.vue';
import SettingSwitch from './SettingSwitch.vue';
import UiIcon from './UiIcon.vue';
const props = defineProps({ updates: Object, moduleInfo: Object, active: Boolean });
const dialog = ref(null), trigger = ref(null), confirmVersion = ref('');
const titles = { idle: '检查是否有新版本', checking: '正在检查更新', current: '已是最新版本',
  available: '发现新版本', ready: '更新已下载', downloading: '正在下载并校验', installing: '正在安装更新',
  reboot: '安装完成，等待软重启', paused: '更新状态待核实', error: '暂时无法检查更新' };
const time = value => new Date(value).toLocaleString('zh-CN', { month: 'numeric', day: 'numeric', hour: '2-digit', minute: '2-digit' });
const canUpdate = computed(() => ['available', 'ready'].includes(props.updates.phase));
const channel = computed(() => props.updates.available && !props.updates.reboot ? props.updates.release.preview ? '预览版' : '正式版' : props.updates.readyChannel === 'preview' ? '预览版' : '正式版');
function request(event) { trigger.value = event.currentTarget; confirmVersion.value = props.updates.target; props.updates.requestUpgrade(); }
function cancel() { props.updates.cancelUpgrade(); }
async function confirm() {
  if (props.updates.target !== confirmVersion.value) { cancel(); return; }
  await props.updates.upgrade();
}
watch(() => [props.active, props.updates.confirming], async ([active, confirming]) => {
  await nextTick();
  if (!dialog.value) return;
  if (active && confirming && canUpdate.value) { if (!dialog.value.open) dialog.value.showModal(); }
  else { if (dialog.value.open) dialog.value.close(); if (!active && confirming) cancel(); }
}, { flush: 'post' });
function closed() { cancel(); if (props.active && trigger.value?.isConnected) trigger.value.focus({ preventScroll: true }); }
onBeforeUnmount(() => { dialog.value?.close(); });
</script>

<template>
  <section id="settings-updates" class="card update-card" aria-labelledby="updateTitle">
    <div class="section-heading"><h2 id="updateTitle">模块更新</h2><span class="badge">当前 v{{ moduleInfo.version }}</span></div>
    <SettingSwitch id="updatePreview" label="包含预览版" help="预览版可能尚不稳定" :checked="updates.preview" :disabled="updates.busy || updates.checking" @change="updates.setPreview" />
    <div class="update-status" :data-kind="updates.kind" :aria-busy="updates.checking || updates.busy">
      <span class="update-status-icon"><UiIcon :name="updates.phase === 'reboot' ? 'info' : canUpdate ? 'download' : 'shield'" /></span>
      <div><b>{{ titles[updates.phase] || '模块更新' }}</b><p id="moduleUpdateStatus" role="status">{{ updates.message }}</p></div>
    </div>
    <div v-if="updates.target" class="update-target">
      <div><small>{{ updates.phase === 'reboot' ? '已安装版本' : '目标版本' }}</small><b>v{{ updates.target }}</b></div><span class="badge">{{ channel }}</span>
    </div>
    <p v-if="updates.phase === 'ready'" class="subtle">安装包已下载并通过 SHA-256 校验，确认后即可安装。</p>
    <p v-if="updates.phase === 'reboot'" class="subtle">请自行软重启设备，然后重新打开模块核实新版本。</p>
    <p v-if="updates.checkedAt && !updates.busy && updates.phase !== 'reboot'" class="sync-note update-cache-note">{{ updates.checkCached ? '上次检查结果 · ' : '检查于 · ' }}{{ time(updates.checkedAt) }}{{ updates.checkCached ? '，更新前会重新核验' : '' }}</p>
    <div class="update-actions">
      <ActionButton v-if="updates.phase === 'paused'" id="resumeModuleJob" icon="sync" variant="primary" :busy="updates.busy" :disabled="updates.checking" @click="updates.resume">继续查询更新状态</ActionButton>
      <ActionButton v-else-if="updates.busy" id="moduleUpdateWorking" icon="download" variant="primary" busy>{{ updates.phase === 'installing' ? '正在安装…' : '正在下载并校验…' }}</ActionButton>
      <ActionButton v-else-if="canUpdate" id="updateModule" icon="download" aria-haspopup="dialog" aria-controls="moduleUpdateDialog" variant="primary" @click="request">{{ updates.phase === 'ready' ? '安装更新' : '更新到 v' + updates.target }}</ActionButton>
      <ActionButton v-if="!updates.busy && !['paused', 'reboot'].includes(updates.phase)" id="getUpdate" icon="sync" :variant="canUpdate ? 'ghost' : 'primary'" :busy="updates.checking" @click="updates.check">{{ updates.checking ? '检查中…' : canUpdate ? '重新检查' : '检查更新' }}</ActionButton>
    </div>
    <dialog ref="dialog" id="moduleUpdateDialog" class="update-dialog" aria-labelledby="moduleUpdateConfirmTitle" @keydown.esc.stop @cancel.prevent="cancel" @close="closed">
      <h2 id="moduleUpdateConfirmTitle">更新到 v{{ confirmVersion }}？</h2>
      <p>{{ updates.phase === 'ready' ? '已校验的安装包将交由当前根管理器安装。' : '将下载安装包，校验通过后交由当前根管理器安装。' }}更新会保留规则、名单和设置。</p>
      <p class="subtle">安装完成后，需要你手动软重启设备。</p>
      <div class="update-actions"><ActionButton id="confirmModuleInstall" variant="primary" @click="confirm">确认更新</ActionButton><button id="cancelModuleInstall" autofocus type="button" class="btn" @click="cancel">取消</button></div>
    </dialog>
  </section>
</template>
