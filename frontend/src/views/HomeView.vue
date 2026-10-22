<script setup>
import ActionButton from '../components/ActionButton.vue';
import SettingSwitch from '../components/SettingSwitch.vue';
import DiagnosticConsole from '../components/DiagnosticConsole.vue';
import UiIcon from '../components/UiIcon.vue';
defineProps({ engine: Object, sync: Object, apps: Object, feedback: Object, moduleInfo: Object });
</script>

<template>
  <div class="view-content">
    <div class="page-heading overview-heading"><h1>守护概览</h1><span id="moduleVersion" class="version-badge" :title="moduleInfo.verified ? '已读取本机模块版本' : '随包界面版本，尚未核实引擎版本'">v{{ moduleInfo.version }}<span v-if="!moduleInfo.verified" class="sr-only"> · 界面版本，未核实引擎</span></span></div>
    <div class="home-grid">
      <section class="card hero" :data-state="engine.kind">
        <div class="hero-top"><span id="stateBadge" class="badge" :class="engine.kind" :data-kind="engine.kind" role="status">{{ engine.badge }}</span></div>
        <div class="hero-heading"><h2 id="stateTitle" :data-kind="engine.kind">{{ engine.title }}</h2><div class="guard-emblem" aria-hidden="true"><span v-if="engine.running" class="guard-halo" /><span class="guard-core"><UiIcon :name="engine.running ? 'shield' : 'shield-idle'" /></span></div></div>
        <p id="stateDesc" class="subtle">{{ engine.description }}</p>
        <div v-if="engine.running" class="blocked-summary" aria-label="本次守护规则命中统计">
          <span>累计拦截</span><b id="blockedTotal">{{ engine.blockedTotal === null ? '—' : engine.blockedTotal.toLocaleString() }}</b>
          <small>{{ engine.metricsStale ? '统计暂不可用，保留上次记录' : engine.blockedTotal === null ? '等待统计数据' : '本次守护 · 规则命中次数' }}</small>
        </div>
        <div class="action-grid">
          <ActionButton id="startBtn" variant="primary" icon="play" :busy="engine.action === 'start'" :disabled="!engine.ready || !!engine.action || !!engine.setting || apps.saving || engine.running" @click="engine.control('start')"><span id="startLabel">{{ engine.action === 'start' ? '启动中…' : '启动守护' }}</span></ActionButton>
          <ActionButton id="stopBtn" icon="pause" :busy="engine.action === 'stop'" :disabled="!engine.ready || !!engine.action || !!engine.setting || apps.saving || (!engine.state?.running && !engine.state?.enabled)" @click="engine.control('stop')"><span id="stopLabel">{{ engine.action === 'stop' ? '暂停中…' : '暂停守护' }}</span></ActionButton>
        </div>
      </section>
      <section class="card sync-card">
        <div class="section-heading"><h2>本地规则库</h2></div>
        <div class="metric-grid">
          <div class="metric"><b id="ruleTotal">{{ engine.total === null ? '—' : engine.total.toLocaleString() }}</b><span>当前模式规则</span></div>
          <div class="metric"><b id="allowTotal">{{ apps.selected.size }} 个</b><span id="appMetricLabel">{{ apps.noun }}应用</span></div>
        </div>
        <ActionButton id="syncBtn" icon="sync" :busy="sync.busy" :disabled="!engine.ready" @click="sync.start"><span id="syncLabel">{{ sync.busy ? '正在同步…' : '同步开源规则' }}</span></ActionButton>
        <p id="updateStatus" class="sync-note" :data-kind="sync.kind" role="status">{{ sync.message }}</p>
      </section>
      <section class="card settings-card">
        <h2>拦截与启动设置</h2>
        <fieldset :disabled="!engine.ready || !!engine.setting || !!engine.action || apps.saving">
          <legend class="field-title">规则强度</legend>
          <div id="modeSeg" class="seg" aria-label="规则强度">
            <button v-for="(label, n) in ['仅 Hosts', 'Hosts + AdGuard']" :key="n" type="button" :data-mode="n + 1" :aria-pressed="engine.state?.mode === n + 1" :class="{ active: engine.state?.mode === n + 1 }" @click="engine.changeSetting('mode', n + 1)">{{ label }}</button>
          </div>
          <p class="field-title">拦截范围</p>
          <div id="scopeSeg" class="seg" aria-label="拦截范围">
            <button v-for="(label, n) in ['全局 · 名单放行', '仅拦截所选应用']" :key="n" type="button" :data-scope="n" :aria-pressed="engine.scope === n" :class="{ active: engine.scope === n }" @click="engine.changeSetting('scope', n)">{{ label }}</button>
          </div>
          <p class="subtle setting-note">两种范围使用独立应用名单，请切换后到“应用名单”勾选并保存。仅 Hosts 不加载 AdGuard 分类，自定义黑白规则始终保留。</p>
          <div class="settings-list">
            <SettingSwitch id="autostart" label="开机自动启动" help="默认关闭；开启后仍尊重手动暂停，不影响当前运行状态。" :checked="!!engine.state?.autostart" @change="engine.changeSetting('autostart', $event)" />
            <SettingSwitch id="quic" label="阻止 QUIC" help="默认开启；阻止 UDP 443，促使应用回退到 TCP。" :checked="!!engine.state?.block_quic" @change="engine.changeSetting('quic', $event)" />
            <SettingSwitch id="devLog" label="DNS 流量日志" help="默认开启；域名日志仅存本机，可随时关闭。" :checked="!!engine.state?.dev_mode" @change="engine.changeSetting('dev', $event)" />
          </div>
        </fieldset>
      </section>
      <DiagnosticConsole :feedback="feedback" />
    </div>
  </div>
</template>
