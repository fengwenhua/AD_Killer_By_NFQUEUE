#!/usr/bin/env python3
"""Optional browser regression: all Android bridge responses are mocked."""
import argparse
from importlib.metadata import version
import json
from pathlib import Path
from playwright.sync_api import expect, sync_playwright


ROOT = Path(__file__).resolve().parents[1]
PROPERTIES = dict(line.split('=', 1) for line in (ROOT / 'module/module.prop').read_text().splitlines() if '=' in line)
VERSION_FIXTURE = {'module': PROPERTIES['id'], 'version': PROPERTIES['version'].removeprefix('v'), 'versionCode': int(PROPERTIES['versionCode']), 'releaseChannel': PROPERTIES.get('releaseChannel', 'stable')}


def luminance(color):
    if len(color) == 4:
        color = "#" + "".join(channel * 2 for channel in color[1:])
    rgb = [int(color[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    linear = [value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4
              for value in rgb]
    return sum(value * weight for value, weight in zip(linear, (0.2126, 0.7152, 0.0722)))


MOCK = r'''
window.previewCommands = [];
window.previewJobs = [];
window.previewJobQueryFails = false;
window.previewJobDiscoveryFails = false;
window.previewJobOverrides = {};
window.previewUpdateFails = false;
window.previewPartial = false;
window.previewSaveFails = false;
window.previewSaveDelay = 0;
window.previewTestFails = false;
window.previewContentFails = false;
window.previewContentDelay = 0;
window.previewVersion = __VERSION_FIXTURE__;
window.previewModuleReady = "";
window.previewModuleReboot = false;
window.previewModuleCheckDelay = 0;
window.previewModuleCheckFails = false;
window.previewNativeBusy = [];
window.previewReleaseNotesFails = false;
window.previewRulesEmpty = false;
window.previewEngineState = "stopped";
window.previewRoutingActive = true;
window.previewFreezerState = 'normal';
window.previewCleanupCode = '';
window.previewDaemonImageCurrent = true;
window.previewStartFails = true;
window.previewStartDelay = 0;
window.previewMetrics = {status:'current',owner_verified:true,owner_pid:123,owner_started:100,owner_boot:'12345678-1234-1234-1234-123456789abc',age_ms:0,updated_at:1792610000,block_suffix_hits:120,block_exact_hits:34,blocked_retransmits:999,quic_rejects:99};
window.previewSettings = {mode:2,scope:0,autostart:false,block_quic:false,dev_mode:false};
window.previewApplySerial=1;
window.previewApplyResult='applied';
window.previewApply={status:'applied',verified:true,verification:'owner_and_commit',desired_revision:'a'.repeat(64),applied_revision:'a'.repeat(64),mode:2,scope:0,block_quic:false,error:''};
window.previewSubmitApply=()=>{
  const running=previewEngineState==='running', revision=(++previewApplySerial).toString(16).padStart(64,'0');
  const complete=running && previewApplyResult==='applied';
  previewApply={...previewApply,status:running?previewApplyResult:'deferred',desired_revision:revision,
    ...(complete?{...previewSettings,applied_revision:revision,verified:true,error:''}:{})};
  return JSON.stringify({status:running?'queued':'deferred',revision});
};
window.previewEngineError = "NFQUEUE 队列 0 已被 YH_hosts 占用，请先停用旧模块。";
window.previewBuiltinSource={url:"builtin:hagezi-pro-minus-fake",enabled:false,locked:true,name:"HaGeZi Pro − Fake",dependencies:[
  "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro.txt",
  "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/fake.txt"]};
window.previewSources = (__SOURCE_FIXTURE__ || [{url:"https://anti-ad.net/easylist.txt",enabled:true},
  {url:"https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt",enabled:false},{...previewBuiltinSource}])
  .map(s=>s.url===previewBuiltinSource.url?{...previewBuiltinSource,...s}:s);
window.ksu = {
  listPackages() { return '["org.example.browser","org.example.chat"]'; },
  getPackagesInfo() { return JSON.stringify([
    {packageName:"org.example.browser",appLabel:"浏览器"},
    {packageName:"org.example.chat",appLabel:"消息"}]); },
  exec(command, options, callback) {
    previewCommands.push(command);
    let text = "", errno = 0;
    if(command.includes(" --version-status")) text = JSON.stringify(previewVersion);
    else if(command.includes(" --jobs-list")) {
      errno=previewJobDiscoveryFails?1:0;text=JSON.stringify(errno?{status:'unverified'}:{status:'ok',jobs:previewJobs});
    }
    else if(command.includes(" --config-read ")) {
      const target=command.split(' ').pop();
      text=JSON.stringify({status:'ok',target,revision:'a'.repeat(64),content:target==='custom'?'ads.example.com\n@@safe.example.org\n':''});
    }
    else if(command.includes(" --config-save ")) {
      const target=command.match(/--config-save (custom|allow|intercept)/)[1];
      previewApply={...previewApply,status:'saved',desired_revision:(++previewApplySerial).toString(16).padStart(64,'0')};
      text=JSON.stringify({status:'ok',target,revision:'b'.repeat(64)});
    }
    else if(command.includes(" --module-update-status")) text=JSON.stringify({version:previewModuleReady,rebootRequired:previewModuleReboot});
    else if(command.includes(" --module-release-notes")) {
      previewNativeBusy.push({kind:'notes',busy:document.querySelector('#reloadReleaseNotes')?.getAttribute('aria-busy')==='true'});
      errno=previewReleaseNotesFails?1:0;
      text=JSON.stringify(errno?{status:'error',http_status:403,message:'GitHub API 访问受限（HTTP 403），当前网络可能已用完匿名请求配额。'}:[{tag_name:"v9.0",draft:false,prerelease:true,name:"最新发布说明",published_at:"2026-10-21T08:00:00Z",html_url:"https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases/tag/v9.0",body:"来自 GitHub Release 的更新日志\n\n- 新版改进\n- 安全显示 <img src=x onerror=alert(1)>"}]);
    }
    else if(command.includes(" --module-update-check")) {
      previewNativeBusy.push({kind:'check',busy:document.querySelector('#getUpdate')?.getAttribute('aria-busy')==='true'});
      const version="9.0", name="AD_Killer_By_NFQUEUE-v9.0.zip", base="https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases";
      text=JSON.stringify({tag_name:"v"+version,draft:false,prerelease:false,html_url:base+"/tag/v"+version,body:"测试发布说明 <img src=x onerror=alert(1)>",assets:[{name,size:500,browser_download_url:base+"/download/v"+version+"/"+name},{name:name+".sha256",browser_download_url:base+"/download/v"+version+"/"+name+".sha256"}]});
      if(previewModuleCheckFails){errno=1;text=JSON.stringify({status:'error',http_status:403,message:'GitHub API 访问受限（HTTP 403），当前网络可能已用完匿名请求配额。'});}
    }
    else if(command.includes(" --module-update-download ")) {previewModuleReady="9.0";text=JSON.stringify({status:"started",job_id:"987-654"});}
    else if(command.includes(" --module-update-install ")) {previewModuleReboot=true;text=JSON.stringify({status:"started",job_id:"987-655"});}
    else if(command.includes(' --apply-status ')) text=JSON.stringify({...previewApply,
      status:command.split(' ').pop()!==previewApply.desired_revision?'superseded':previewApply.status});
    else if(command.includes(" --status")) text = JSON.stringify({
      iptables_active:previewEngineState==="running" && previewRoutingActive, running:previewEngineState==="running",
      daemon_image_current:previewDaemonImageCurrent,
      freezer_state:previewFreezerState,frozen:previewFreezerState==='unknown'?null:previewFreezerState==='frozen',
      last_error_code:previewCleanupCode,
      runtime:previewEngineState, last_error:previewEngineState==="error"?previewEngineError:"",
      enabled:previewEngineState!=="stopped",daemon_pid:123, metrics:previewMetrics,
      rules_black:previewRulesEmpty?0:185695,rules_white:previewRulesEmpty?0:1062,
      apply:previewEngineState==='running'?previewApply:{...previewApply,status:'deferred',verified:false}, ...previewSettings});
    else if(command.endsWith(" --start")) {
      if(previewRulesEmpty){errno=1;text=JSON.stringify({status:'error_rules_empty',message:'请先点击“同步开源规则”，同步成功后再启动守护。'});}
      else {previewEngineState=previewStartFails?"error":"running";
      if(!previewStartFails)previewSubmitApply();
      errno=previewStartFails?1:0;
      text=JSON.stringify({status:previewStartFails?"error_queue_conflict":"starting",message:previewStartFails?previewEngineError:""});}
    }
    else if(command.endsWith(" --stop")) {previewEngineState="stopped";previewCleanupCode='';text='{"status":"stopped"}';}
    else if(command.includes(' --autostart ')) {previewSettings.autostart=command.endsWith(' on');text=JSON.stringify({autostart:previewSettings.autostart});}
    else if(command.includes(' --quic ')) {previewSettings.block_quic=command.endsWith(' on');text=previewSubmitApply();}
    else if(command.includes(' --dev ')) {previewSettings.dev_mode=command.endsWith(' on');text=previewSubmitApply();}
    else if(command.includes(' --scope ')) {previewSettings.scope=Number(command.split(' ').pop());text=previewSubmitApply();}
    else if(command.includes(' --mode ')) {previewSettings.mode=Number(command.split(' ').pop());text=previewSubmitApply();}
    else if(command.includes(' --apps-apply') || command.includes(' --reload')) text=previewSubmitApply();
    else if(command.includes(" --sources-list")) text=JSON.stringify({status:"ok",sources:previewSources});
    else if(command.includes(" --sources-save")) {
      if(previewSaveFails){setTimeout(()=>window[callback](2,"","规则同步正在运行"),0);return}
      const encoded=command.match(/^printf '%s' '([A-Za-z0-9+/=]+)'/)[1];
      previewSources=decodeURIComponent(escape(atob(encoded))).trim().split("\n").filter(Boolean).map(line=>({
        enabled:!line.startsWith("# disabled: "),url:line.replace(/^# disabled: /,"")})).map(s=>s.url===previewBuiltinSource.url?{...previewBuiltinSource,...s}:s);
      text=JSON.stringify({status:"ok",sources:previewSources});
    }
    else if(command.includes(" --source-preview")) {
      const urls = command.includes('builtin:hagezi-pro-minus-fake') ? previewBuiltinSource.dependencies : [command.match(/--source-preview '([^']+)'/)[1]];
      errno=previewContentFails?1:0;
      text=JSON.stringify({status:errno?'error':'ok',previews:urls.map(url=>({url,
        text:previewContentFails?'':'<script>window.previewInjected=true</script>中😀'.repeat(200),
        error:previewContentFails?'TLS verification failed':'',truncated:true}))});
    }
    else if(command.includes(" --source-test")) text = '{"status":"started","job_id":"777-888"}';
    else if(command.includes(" --update-start")) text = '{"status":"started","job_id":"123-456"}';
    else if(command.includes(" --update-status")) {
      const id=command.match(/--update-status ([0-9-]+)/)[1], failed=id==='777-888'?previewTestFails:previewUpdateFails;
      errno=previewJobQueryFails?1:0;
      text=JSON.stringify(errno?{status:'unverified',job_id:id}:previewJobOverrides[id]||{status:failed?'failed':'success',exit_code:failed?1:0,job_id:id,
        type:id==='777-888'?'source':id==='987-654'?'download':id==='987-655'?'install':'rules'});
    }
    else if(command.includes(" --update-log")) {
      const match=command.match(/--update-log '([0-9-]+)'(?: '([^']*)')?/), id=match[1], cursor=match[2]||'';
      const body=id==='777-888'?'来源接受:Hosts 0 / AdGuard 12 / 拒绝 2\n来源测试通过；仅测试下载和解析，未替换规则或启动引擎':id==='123-456'?(previewUpdateFails?'[12:00:00] [更新] staging 规则无法被引擎加载,保留旧规则\n':'[12:00:00] [更新] 规则 generation 999-111 已原子发布\n'):'模块下载/安装已结束';
      const encoded=btoa(unescape(encodeURIComponent(body))), end='preview-'+id+':'+new TextEncoder().encode(body).length;
      text=JSON.stringify({status:'ok',data_base64:cursor===end?'':encoded,cursor:end,reset:!cursor||cursor==='tail',reset_reason:cursor==='tail'?'tail':'begin',more:false,omitted_bytes:0});
    }
    else if(command.includes("base64 ")) {
      if(command.includes("rule_stats.conf")) text=previewRulesEmpty?'':btoa("black=185695\nwhite=1062\n");
      else if(command.includes("source_stats.conf")) text=btoa("updated_at=1792410474\njob_id=123-456\npublish_status=published\ngeneration=999-111\nsource_count=2\naccepted_sources="+(previewPartial?1:2)+"\nunique_total=209084\nunique_white=119\nduplicate_lines=73170\n\n[source_0]\nurl=https://anti-ad.net/easylist.txt\nstatus="+(previewPartial?"download_error\nreason=network_timeout":"accepted")+"\nchecked_at=1792410460\naccepted_adguard=96641\nrejected_unsupported=97\n\n[source_1]\nurl=https://example.org/b\nstatus=accepted\naccepted_adguard=185613\nrejected_unsupported=798\n");
      else if(command.includes("custom_rules.txt")) text=btoa("ads.example.com\n@@safe.example.org\n");
      else if(command.includes("update-123-456.log")) text=btoa(unescape(encodeURIComponent(previewUpdateFails ?
        "[12:00:00] [更新] staging 规则无法被引擎加载,保留旧规则\n" :
        "[12:00:00] [更新] 规则 generation 999-111 已原子发布\n")));
      else if(command.includes("update-777-888.log")) text=btoa(unescape(encodeURIComponent("来源接受:Hosts 0 / AdGuard 12 / 拒绝 2\n来源测试通过；仅测试下载和解析，未替换规则或启动引擎")));
    }
    setTimeout(()=>window[callback](errno,text,""),command.includes(" --sources-save")?previewSaveDelay:command.includes(' --source-preview')?previewContentDelay:command.includes(' --module-update-check')?previewModuleCheckDelay:command.endsWith(' --start')?previewStartDelay:0);
  }
};
'''


def mock_script(sources=None):
    # Inject fixtures with the bridge in ONE init script, before Vue mounts.
    # Do not transport test fixtures through file:// sessionStorage or depend
    # on the undefined ordering of multiple Playwright init scripts.
    return MOCK.replace('__SOURCE_FIXTURE__', json.dumps(sources)).replace('__VERSION_FIXTURE__', json.dumps(VERSION_FIXTURE))


ORDERING_SOURCES = [
    {'url': 'https://example.org/first', 'enabled': True},
    {'url': 'https://example.org/disabled', 'enabled': False},
    {'url': 'https://example.org/middle', 'enabled': True},
    {'url': 'builtin:hagezi-pro-minus-fake', 'enabled': False},
    {'url': 'https://example.org/last', 'enabled': True},
]


def fixture_regression(browser, errors):
    # A fresh file document may have empty or unavailable sessionStorage.
    # Neither condition is allowed to alter the mocked backend source list.
    for storage in ('cleared', 'denied'):
        page = browser.new_page()
        page.on('pageerror', lambda error: errors.append(str(error)))
        setup = "try{sessionStorage.clear()}catch{}" if storage == 'cleared' else "Object.defineProperty(window,'sessionStorage',{get(){throw new Error('sessionStorage denied')}});"
        page.add_init_script(setup + '\n' + mock_script(ORDERING_SOURCES))
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        for load in ('initial', 'reload'):
            if load == 'reload':
                page.reload()
            expect(page.locator('.source-item'), f'{storage} sessionStorage, {load}').to_have_count(5)
            assert page.locator('.source-item[data-builtin="true"]').count() == 1
            assert page.evaluate('previewSources.length') == 5
        page.close()


def apply_health_regression(browser, errors):
    for theme in ('light', 'dark'):
        page = browser.new_page(viewport={'width': 320, 'height': 740}, color_scheme=theme, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script() + "\npreviewEngineState='running';previewSettings.scope=1;previewSettings.mode=1;previewSettings.block_quic=true;previewApply={...previewApply,status:'failed',desired_revision:'f'.repeat(64),error:'IPv6 switch failed <img src=x onerror=alert(1)>'};")
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#stateBadge')).to_have_text('应用失败')
        expect(page.locator('#stateDesc')).to_contain_text('此前策略仍在运行')
        expect(page.locator('#appliedPolicy')).to_contain_text('Hosts + AdGuard · 全局 · QUIC 放行')
        expect(page.locator('#startBtn')).to_be_disabled()
        expect(page.locator('#ruleTotal')).to_have_text('186,757')
        assert page.locator('#stateDesc img').count() == 0
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
        page.evaluate('previewCommands.length=0')
        page.locator('#retryApplyBtn').click()
        expect(page.locator('#retryApplyBtn')).to_have_attribute('aria-busy', 'true')
        expect(page.locator('#stateBadge')).to_have_text('正在守护')
        expect(page.locator('#retryApplyBtn')).to_have_count(0)
        assert page.evaluate("previewCommands.some(c=>c.includes(' --apps-apply')) && previewCommands.some(c=>c.includes(' --apply-status '))")
        assert page.evaluate("!previewCommands.some(c=>/ --(?:start|stop|daemon)$/.test(c))")
        # A healthy-looking process and route alone are not startup/config ACKs.
        page.evaluate("previewApply={...previewApply,status:'unverified',verified:false}")
        page.locator('nav [data-page="settings"]').click()
        page.locator('nav [data-page="home"]').click()
        expect(page.locator('#stateBadge')).to_have_text('生效待确认')
        expect(page.locator('#startBtn')).to_be_disabled()
        expect(page.locator('#verifyHealthBtn')).to_be_visible()
        page.close()


def background_job_recovery_regression(browser, errors):
    for theme in ('light', 'dark'):
        page = browser.new_page(viewport={'width': 320, 'height': 852}, color_scheme=theme, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script() + r'''
Object.defineProperty(window,'localStorage',{get(){throw new Error('storage denied')}});
previewModuleReady='9.0';
previewJobs=[{job_id:'777-888',type:'source',status:'running',target:'https://unsaved.example.org/'+('long'.repeat(80))+'?x=<img/src=x/onerror=alert(1)>'},
 {job_id:'987-654',type:'download',status:'running',target:'9.0',channel:'stable'}];
''')
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        page.locator('nav [data-page="rules"]').click()
        page.locator('#sourcesDetails > summary').click()
        expect(page.locator('.source-history').filter(has_text='恢复的来源测试：')).to_have_count(1)
        expect(page.locator('[aria-label="恢复的来源测试日志"]')).to_contain_text('来源测试通过')
        assert page.locator('#sourceList .source-item').count() == 3, 'recovery added an unsaved test URL to configured sources'
        assert page.locator('#rules img').count() == 0
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
        page.locator('nav [data-page="settings"]').click()
        page.locator('[data-settings-entry="updates"]').click()
        expect(page.locator('#moduleUpdateStatus')).to_contain_text('下载完成')
        assert not page.evaluate("previewCommands.some(c=>/ --(?:update-start|source-test|module-update-download|module-update-install)\b/.test(c))")
        assert page.evaluate("previewCommands.some(c=>c.includes(' --update-log '))")
        page.close()

    page = browser.new_page(viewport={'width': 320, 'height': 852}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script() + '\npreviewJobDiscoveryFails=true;previewModuleReady="9.0";')
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#syncLabel')).to_have_text('继续查询同步')
    page.locator('#syncBtn').click()
    expect(page.locator('#syncBtn')).to_have_attribute('aria-busy', 'false')
    assert not page.evaluate("previewCommands.some(c=>c.endsWith(' --update-start'))")
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    expect(page.locator('#resumeSourceTest')).to_be_visible()
    page.locator('#resumeSourceTest').click()
    page.locator('nav [data-page="settings"]').click()
    page.locator('[data-settings-entry="updates"]').click()
    expect(page.locator('#resumeModuleJob')).to_be_visible()
    assert page.locator('#installModuleUpdate').count() == 0
    page.locator('#resumeModuleJob').click()
    assert not page.evaluate("previewCommands.some(c=>/ --(?:source-test|module-update-download|module-update-install)\b/.test(c))")
    page.evaluate('previewJobDiscoveryFails=false')
    page.locator('nav [data-page="home"]').click()
    page.locator('#syncBtn').click()
    expect(page.locator('#syncLabel')).to_have_text('同步开源规则')
    assert not page.evaluate("previewCommands.some(c=>c.endsWith(' --update-start'))"), 'read-only recovery silently created a task'
    page.locator('#syncBtn').click()
    expect(page.locator('#updateStatus')).to_have_attribute('data-kind', 'success')
    assert page.evaluate("previewCommands.filter(c=>c.endsWith(' --update-start')).length") == 1
    page.close()


def freezer_health_regression(browser, errors):
    page = browser.new_page(viewport={'width': 320, 'height': 740}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script() + "\npreviewEngineState='running'; previewFreezerState='unknown';")
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#stateBadge')).to_have_text('状态未核实')
    expect(page.locator('#startBtn')).to_be_disabled()
    expect(page.locator('#blockedTotal')).to_have_count(0)
    page.evaluate("previewCommands.length=0")
    page.locator('#verifyHealthBtn').click()
    expect(page.locator('#verifyHealthBtn')).to_have_attribute('aria-busy', 'false')
    assert page.evaluate("previewCommands.every(c=>c.includes('--status') || c.includes('rule_stats.conf'))")
    page.evaluate("previewFreezerState='normal'")
    page.locator('#verifyHealthBtn').click()
    expect(page.locator('#stateBadge')).to_have_text('正在守护')
    expect(page.locator('#verifyHealthBtn')).to_have_count(0)
    page.close()


def cleanup_health_regression(browser, errors):
    page = browser.new_page(viewport={'width': 320, 'height': 740}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script() + "\npreviewEngineState='stopped'; previewCleanupCode='cleanup_unknown';")
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#stateTitle')).to_have_text('停止结果待核实')
    expect(page.locator('#stateBadge')).to_have_text('清理待核实')
    expect(page.locator('#startBtn')).to_be_disabled()
    expect(page.locator('#stopBtn')).to_be_enabled()
    page.locator('#stopBtn').click()
    expect(page.locator('#stateBadge')).to_have_text('已暂停')
    assert page.evaluate("!previewCommands.some(c=>c.endsWith(' --start') || / --reload(?: |$)/.test(c))")
    page.close()


def preview_regression(browser, errors, screenshots=None):
    page = browser.new_page(viewport={'width': 320, 'height': 740}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script())
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#ruleTotal')).to_have_text('186,757')
    # Font fallback differs across Android/Linux/macOS. A million-rule count
    # must also fit a single line at the narrowest supported viewport.
    page.locator('#ruleTotal').evaluate("e=>e.textContent='1,000,000'")
    assert page.locator('#ruleTotal').evaluate('e=>e.getBoundingClientRect().height <= parseFloat(getComputedStyle(e).lineHeight) + 1')
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    ordinary = page.locator('.source-item').first
    initial_sources = page.evaluate('JSON.stringify(previewSources)')
    initial_history = ordinary.locator('.source-history').all_text_contents()
    assert ordinary.locator('.source-actions button').all_text_contents() == ['编辑', '查看内容', '测试', '删除']
    page.evaluate('previewContentDelay=600')
    original_height = ordinary.bounding_box()['height']
    ordinary.get_by_role('button', name='查看内容', exact=True).click()
    dialog = page.get_by_role('dialog', name='来源内容预览')
    expect(dialog).to_be_visible()
    expect(dialog.get_by_role('status').filter(has_text='正在读取')).to_be_visible()
    assert ordinary.bounding_box()['height'] == original_height, 'preview must not expand the rule list'
    expect(dialog.locator('pre')).to_be_visible()
    content = dialog.locator('pre').inner_text()
    assert len(content) == 500 and '<script>' in content
    assert not page.evaluate('!!window.previewInjected'), 'remote source executed as HTML'
    assert dialog.locator('script').count() == 0
    assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
    count = page.evaluate("previewCommands.filter(c=>c.includes(' --source-preview')).length")
    assert ordinary.get_by_role('button', name='查看内容', exact=True).get_attribute('aria-haspopup') == 'dialog'
    page.keyboard.press('Escape')
    expect(dialog).not_to_be_visible()
    assert ordinary.get_by_role('button', name='查看内容', exact=True).evaluate('e=>document.activeElement===e')
    ordinary.get_by_role('button', name='查看内容', exact=True).click()
    assert page.evaluate("previewCommands.filter(c=>c.includes(' --source-preview')).length") == count
    page.evaluate('previewContentFails=true')
    dialog.get_by_role('button', name='重新获取', exact=True).click()
    expect(dialog).to_contain_text('TLS verification failed')
    page.evaluate('previewContentFails=false')
    dialog.get_by_role('button', name='重新获取', exact=True).click()
    expect(dialog.locator('pre')).to_be_visible()
    if screenshots:
        page.screenshot(path=str(screenshots / 'source-preview-320.png'))
    dialog.get_by_role('button', name='关闭内容预览').click()
    special = page.locator('.source-item[data-builtin="true"]')
    special.get_by_role('button', name='查看内容', exact=True).click()
    expect(dialog.locator('pre')).to_have_count(2)
    expect(dialog).to_contain_text('不是差集结果')
    assert special.get_by_role('button', name='编辑', exact=True).is_disabled()
    assert special.get_by_role('button', name='删除', exact=True).is_disabled()
    assert page.evaluate('JSON.stringify(previewSources)') == initial_sources
    assert ordinary.locator('.source-history').all_text_contents() == initial_history
    assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|source-test|update-start|sources-save)(?: |$)/.test(c))")
    if screenshots:
        page.screenshot(path=str(screenshots / 'builtin-preview-320.png'))
    for button in dialog.locator('button:visible').all():
        box = button.bounding_box()
        assert box['height'] >= 44 and box['width'] >= 44
    # The modal traps focus, and neither the nav nor a background source can
    # receive keyboard focus while it is open.
    for _ in range(6):
        page.keyboard.press('Tab')
        assert page.evaluate("!!document.activeElement.closest('dialog')")
    dialog.get_by_role('button', name='关闭内容预览').click()
    page.locator('nav [data-page="settings"]').click()
    page.locator('[data-settings-entry="preview"]').click()
    expect(page.locator('#previewCharacters')).to_have_value('500')
    page.locator('#previewCharacters').fill('99')
    page.locator('#savePreviewSettings').click()
    expect(page.locator('#previewSettingsStatus')).to_contain_text('整数')
    page.locator('#previewCharacters').fill('1000')
    page.locator('#savePreviewSettings').click()
    expect(page.locator('#previewSettingsStatus')).to_contain_text('已保存')
    page.reload()
    page.locator('nav [data-page="settings"]').click()
    page.locator('[data-settings-entry="preview"]').click()
    expect(page.locator('#previewCharacters')).to_have_value('1000')
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    ordinary.get_by_role('button', name='查看内容', exact=True).click()
    expect(dialog.locator('pre')).to_be_visible()
    assert len(dialog.locator('pre').inner_text()) == 1000
    assert page.evaluate("previewCommands.some(c=>c.includes(' --source-preview ')&&c.endsWith(' 1000'))")
    page.keyboard.press('Escape')
    # Restore default fixture storage for the rest of this context.
    page.evaluate("localStorage.removeItem('ad-killer-by-nfqueue-preview-characters')")
    page.close()


def source_editor_dialog_regression(browser, errors, screenshots):
    for width, height in ((320, 740), (393, 852), (1180, 820)):
        for theme in ('light', 'dark'):
            page = browser.new_page(viewport={'width': width, 'height': height}, color_scheme=theme, reduced_motion='reduce')
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.add_init_script(mock_script() + '''
              window.editorKeyboardHeight=null;window.editorViewportTop=0;
              const viewport=new EventTarget();
              Object.defineProperties(viewport,{height:{get:()=>editorKeyboardHeight??innerHeight},
                offsetTop:{get:()=>editorViewportTop}});
              Object.defineProperty(window,'visualViewport',{value:viewport,configurable:true});
            ''')
            page.goto((ROOT / 'module/webroot/index.html').as_uri())
            expect(page.locator('.app')).to_have_count(2)
            page.locator('nav [data-page="rules"]').click()
            page.locator('#sourcesDetails > summary').click()
            expect(page.locator('.source-item')).to_have_count(3)
            page.locator('#addSource').click()
            dialog = page.get_by_role('dialog', name='新增来源', exact=True)
            expect(dialog).to_be_visible()
            expect(page.locator('#sourceUrl')).to_be_focused()
            assert dialog.evaluate('e=>e.parentElement===document.body && e.matches(":modal")')
            saved_scroll = page.locator('#rules').evaluate('e=>e.scrollTop')
            assert page.locator('#addSource').get_attribute('aria-haspopup') == 'dialog'
            for _ in range(6):
                page.keyboard.press('Tab')
                assert page.evaluate("!!document.activeElement.closest('#sourceEditorDialog')")
            page.keyboard.press('Shift+Tab')
            assert page.evaluate("!!document.activeElement.closest('#sourceEditorDialog')")
            page.locator('#sourceUrl').fill('')
            dialog.get_by_role('button', name='确认地址').click()
            expect(page.locator('#sourceFormStatus')).to_contain_text('地址为空')
            page.locator('#sourceUrl').fill('file:///etc/passwd')
            page.keyboard.press('Enter')
            expect(page.locator('#sourceUrl')).to_have_attribute('aria-invalid', 'true')
            page.locator('#sourceUrl').fill('https://anti-ad.net/easylist.txt')
            page.keyboard.press('Enter')
            expect(page.locator('#sourceFormStatus')).to_contain_text('已存在')
            page.locator('#sourceUrl').fill('https://example.org/modal-rules.txt')
            if screenshots:
                page.screenshot(path=str(screenshots / f'source-editor-{theme}-{width}.png'))
            if width == 393:
                # Keyboard-only visual viewport shrink without changing the
                # layout viewport: the dialog and focused field must still fit.
                page.evaluate('editorKeyboardHeight=300;editorViewportTop=80;visualViewport.dispatchEvent(new Event("resize"))')
                box = dialog.bounding_box()
                assert box['y'] >= 93 and box['y'] + box['height'] <= 367, box
                field = page.locator('#sourceUrl').bounding_box()
                assert field['y'] >= box['y'] and field['y'] + field['height'] <= box['y'] + box['height'], field
                for button in dialog.locator('button').all():
                    control = button.bounding_box()
                    assert control['y'] >= box['y'] and control['y'] + control['height'] <= box['y'] + box['height'], control
                expect(page.locator('#sourceUrl')).to_have_value('https://example.org/modal-rules.txt')
                if screenshots:
                    page.screenshot(path=str(screenshots / f'source-editor-keyboard-{theme}.png'))
                page.evaluate('editorKeyboardHeight=null;editorViewportTop=0;visualViewport.dispatchEvent(new Event("resize"))')
            page.keyboard.press('Escape')
            expect(page.locator('#sourceEditorDialog')).to_have_count(0)
            expect(page.locator('#addSource')).to_be_focused()
            assert page.locator('#rules').evaluate('e=>e.scrollTop') == saved_scroll
            assert page.locator('.source-item').count() == 3
            assert page.locator('#saveSources').is_disabled()
            page.locator('#addSource').click()
            expect(page.locator('#sourceUrl')).to_have_value('')
            page.mouse.click(2, 2)
            expect(page.locator('#sourceEditorDialog')).to_have_count(0)
            page.locator('#addSource').click()
            page.evaluate('''async()=>{
              document.querySelector('#sourceEditorDialog').dispatchEvent(new Event('cancel',{cancelable:true}));
              await Promise.resolve();document.querySelector('#addSource').click();await Promise.resolve();
            }''')
            page.locator('#sourceUrl').fill('https://example.org/modal-rules.txt')
            page.evaluate('new Promise(r=>requestAnimationFrame(()=>requestAnimationFrame(r)))')
            expect(dialog).to_be_visible()
            page.keyboard.press('Enter')
            expect(page.locator('#sourceEditorDialog')).to_have_count(0)
            assert page.locator('.source-item').count() == 4
            assert page.evaluate('previewSources.length') == 3, 'confirm implicitly saved source configuration'
            ordinary = page.locator('.source-item').first
            edit = ordinary.get_by_role('button', name='编辑', exact=True)
            original = ordinary.get_attribute('data-source-url')
            edit.click()
            expect(page.get_by_role('dialog', name='编辑来源', exact=True)).to_be_visible()
            page.locator('#sourceUrl').fill('https://example.org/cancelled-edit')
            page.get_by_role('button', name='关闭来源编辑').click()
            expect(edit).to_be_focused()
            assert ordinary.get_attribute('data-source-url') == original
            assert page.evaluate('previewSources.length') == 3
            assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|source-test|update-start|sources-save)(?: |$)/.test(c))")
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
            page.close()


def motion_and_settings_regression(browser, errors, screenshots):
    for theme in ('light', 'dark'):
        page = browser.new_page(viewport={'width': 393, 'height': 852}, color_scheme=theme)
        requests = []
        page.on('request', lambda request: requests.append(request.url))
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script())
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#moduleVersion')).to_have_text('v' + VERSION_FIXTURE['version'])
        expect(page.locator('.app')).to_have_count(2)
        assert page.locator('nav button').evaluate_all('els=>els.map(e=>e.dataset.page)') == ['home', 'apps', 'rules', 'settings']

        def aligned():
            page.wait_for_function('''() => {
              const a=document.querySelector('nav [aria-current="page"]').getBoundingClientRect(),
                b=document.querySelector('.nav-indicator').getBoundingClientRect();
              return Math.abs(a.x-b.x)<1.1 && Math.abs(a.y-b.y)<1.1 && Math.abs(a.width-b.width)<1.1 && Math.abs(a.height-b.height)<1.1;
            }''')

        # Observe actual native compositor interpolation, not a class-name assertion.
        sample = page.evaluate('''async () => {
          document.querySelector('nav [data-page="apps"]').click();
          const incoming=document.querySelector('#apps'), outgoing=document.querySelector('#home');
          const matrix=e=>new DOMMatrixReadOnly(getComputedStyle(e).transform);
          for(let frame=0;frame<20;frame++) {
            await new Promise(resolve=>requestAnimationFrame(resolve));
            if(matrix(outgoing).m41!==0)break;
          }
          return {incoming:matrix(incoming).m41,outgoing:matrix(outgoing).m41,
            y:[matrix(incoming).m42,matrix(outgoing).m42],
            opacity:[getComputedStyle(incoming).opacity,getComputedStyle(outgoing).opacity],
            width:document.querySelector('main').clientWidth,
            visible:[...document.querySelectorAll('.page')].filter(e=>getComputedStyle(e).visibility==='visible').length,
            interactive:document.querySelectorAll('.page:not([inert])').length};
        }''')
        assert sample['incoming'] > 0 and sample['outgoing'] < 0, sample
        assert abs(sample['incoming'] - sample['outgoing'] - sample['width']) < 1, sample
        assert sample['y'] == [0, 0] and sample['opacity'] == ['1', '1'], sample
        assert sample['visible'] == 2 and sample['interactive'] == 1, sample
        page.wait_for_function("!document.querySelector('main').classList.contains('sliding')")
        aligned()
        theme_sample = page.evaluate('''async () => {
          document.querySelector('.theme-toggle').click();
          await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
          return {theme:document.documentElement.dataset.theme,transform:document.querySelector('.theme-toggle .icon').style.transform};
        }''')
        assert theme_sample['theme'] == ('dark' if theme == 'light' else 'light') and theme_sample['transform']
        page.wait_for_function("!document.querySelector('.theme-toggle .icon').style.transform")
        page.locator('.theme-toggle').click()
        page.wait_for_function("!document.querySelector('.theme-toggle .icon').style.transform")
        page.locator('#appList input').first.check()
        page.locator('nav [data-page="rules"]').click()
        page.locator('#sourcesDetails > summary').click()
        page.locator('#customRulesDetails > summary').click()
        page.locator('#blacklist').fill('motion-draft.example.org')
        page.locator('#source-0').uncheck()
        rapid = page.evaluate('''async () => {
          const samples=[], x=e=>new DOMMatrixReadOnly(getComputedStyle(e).transform).m41;
          for(const id of ['settings','rules','settings','home','apps','apps','rules','home','settings','apps','rules','settings']) {
            const moving=[...document.querySelectorAll('.page')].filter(e=>e.getAnimations().length);
            const before=moving.map(e=>[e,x(e)]);
            document.querySelector(`nav [data-page="${id}"]`).click();
            await Promise.resolve();
            samples.push({id,active:document.querySelector('.page[data-active]')?.id,
              selected:document.querySelector('nav [aria-current=page]')?.dataset.page,
              interactive:[...document.querySelectorAll('.page:not([inert])')].map(e=>e.id),
              jump:Math.max(0,...before.filter(([e])=>getComputedStyle(e).visibility==='visible')
                .map(([e,old])=>Math.abs(x(e)-old))),
              animations:[...document.querySelectorAll('.page')].reduce((n,e)=>n+e.getAnimations().length,0)});
            await new Promise(resolve=>requestAnimationFrame(resolve));
          }
          return samples;
        }''')
        for sample in rapid:
            assert sample['active'] == sample['selected'] == sample['id'], sample
            assert sample['interactive'] == [sample['id']], sample
            assert sample['jump'] < 1, sample
            assert sample['animations'] <= 4, sample
        burst = page.evaluate('''async () => {
          for(const id of ['home','apps','rules','settings','home','settings'])
            document.querySelector(`nav [data-page="${id}"]`).click();
          await Promise.resolve();
          return {active:document.querySelector('.page[data-active]')?.id,
            selected:document.querySelector('nav [aria-current=page]')?.dataset.page,
            animations:[...document.querySelectorAll('.page')].reduce((n,e)=>n+e.getAnimations().length,0)};
        }''')
        assert burst['active'] == burst['selected'] == 'settings', burst
        assert burst['animations'] <= 4, burst
        page.wait_for_function("!document.querySelector('main').classList.contains('sliding') && !!document.querySelector('#settings[data-active]')")
        assert page.locator('.page').evaluate_all('els=>els.every(e=>e.getAnimations().length===0)')
        aligned()
        assert page.locator('.page:visible').count() == 1
        assert page.locator('.page:not([inert])').count() == 1
        assert page.locator('[data-leaving]').count() == 0
        assert page.locator('#blacklist').input_value() == 'motion-draft.example.org'
        assert page.locator('#sourcesDetails').evaluate('e=>e.open')
        assert not page.locator('#source-0').is_checked()
        assert page.locator('#appList input').first.is_checked()
        expect(page.locator('#settingsMenu')).to_be_visible()
        page.locator('[data-settings-entry="about"]').click()
        expect(page.locator('#aboutVersion')).to_contain_text(VERSION_FIXTURE['version'])
        assert page.locator('#versionSource').count() == 0
        page.locator('#settingsBack').click()
        page.locator('[data-settings-entry="updates"]').click()
        expect(page.locator('#getUpdate')).to_be_enabled()
        before = page.evaluate('previewCommands.length')
        page.locator('#getUpdate').click()
        expect(page.locator('#moduleUpdateStatus')).to_contain_text('发现 v9.0')
        assert page.evaluate('previewCommands.length') > before
        expect(page.locator('#downloadModuleUpdate')).to_be_enabled()
        assert page.locator('#settings-updates img').count() == 0
        page.locator('#settingsBack').click()
        assert not any(url.startswith(('http:', 'https:')) for url in requests)
        assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|update-start|sources-save|apps-apply)(?: |$)/.test(c))")

        for width, height in ((744, 1133), (1180, 820), (720, 540), (393, 852)):
            page.set_viewport_size({'width': width, 'height': height})
            aligned()
            assert page.locator('#blacklist').input_value() == 'motion-draft.example.org'
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
        page.emulate_media(reduced_motion='reduce')
        page.locator('nav [data-page="apps"]').click()
        aligned()
        assert page.locator('#apps').evaluate('e=>!e.style.transform && !e.style.opacity')
        page.locator('nav [data-page="settings"]').focus()
        page.keyboard.press('Enter')
        assert page.locator('#settings').is_visible()
        aligned()
        if screenshots:
            page.screenshot(path=str(screenshots / f'settings-{theme}-393.png'), full_page=True)
        page.locator('.theme-toggle').click()
        assert page.evaluate('document.documentElement.dataset.theme') == ('dark' if theme == 'light' else 'light')
        assert page.locator('#blacklist').input_value() == 'motion-draft.example.org'
        page.close()


def scroll_and_push_regression(browser, errors):
    for reduced in ('no-preference', 'reduce'):
        page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion=reduced)
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script(ORDERING_SOURCES) + r'''
          ksu.listPackages=()=>JSON.stringify(Array.from({length:80},(_,i)=>`org.example.app${i}`));
          ksu.getPackagesInfo=packages=>JSON.stringify(JSON.parse(packages).map(packageName=>({packageName,appLabel:packageName})));
        ''')
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#appList')).to_have_attribute('data-total', '80')
        assert page.locator('.app').count() < 40

        def settle():
            page.wait_for_function("!document.querySelector('main').classList.contains('sliding')")

        def navigate(tab):
            # Native clicks on fixed navigation must not scroll a hidden panel
            # into view as a test harness side effect.
            page.locator(f'nav [data-page="{tab}"]').click()
            settle()

        navigate('rules')
        page.locator('#sourcesDetails > summary').click()
        page.locator('#customRulesDetails > summary').click()
        page.locator('#blacklist').fill('scroll-draft.example.org')
        page.locator('#rules').evaluate('e=>e.scrollTop=700')
        rules_y = page.locator('#rules').evaluate('e=>e.scrollTop')
        assert rules_y > 400
        before = page.locator('#rules').bounding_box()['y']
        sample = page.evaluate('''async () => {
          document.querySelector('nav [data-page="home"]').click();
          const old=document.querySelector('#rules'), next=document.querySelector('#home');
          for(let frame=0;frame<20;frame++) {
            await new Promise(resolve=>requestAnimationFrame(resolve));
            if(new DOMMatrixReadOnly(getComputedStyle(old).transform).m41!==0 || !document.querySelector('main').classList.contains('sliding'))break;
          }
          return {scroll:scrollY,top:old.getBoundingClientRect().top,
            oldX:new DOMMatrixReadOnly(getComputedStyle(old).transform).m41,
            nextX:new DOMMatrixReadOnly(getComputedStyle(next).transform).m41};
        }''')
        assert sample['scroll'] == 0, sample
        if reduced == 'no-preference':
            assert abs(sample['top'] - before) < 1, sample
            assert sample['oldX'] > 0 and sample['nextX'] < 0, sample
        settle()
        page.locator('#home').evaluate('e=>e.scrollTop=110')
        home_y = page.locator('#home').evaluate('e=>e.scrollTop')
        navigate('settings')
        assert page.evaluate('scrollY') == 0
        page.locator('[data-settings-entry="releases"]').click()
        page.locator('#settings').evaluate('e=>e.scrollTop=170')
        settings_y = page.locator('#settings').evaluate('e=>e.scrollTop')
        navigate('apps')
        assert page.evaluate('scrollY') == 0
        page.locator('#appList').evaluate('e=>e.scrollTop=310')
        app_y = page.locator('#appList').evaluate('e=>e.scrollTop')
        assert app_y == 310
        page.locator('#appList input').nth(7).check()
        # Checking an offscreen row can scroll the app list; retain the final
        # actual offset, not a hard-coded position from before that action.
        app_y = page.locator('#appList').evaluate('e=>e.scrollTop')
        for tab, expected in (('rules', rules_y), ('home', home_y), ('settings', settings_y), ('rules', rules_y)):
            navigate(tab)
            current = page.locator(f'#{tab}').evaluate('e=>e.scrollTop')
            assert abs(current - expected) <= 1, (reduced, tab, expected, current)
            assert page.evaluate('scrollY') == 0
            assert page.locator('.page:visible').count() == 1
            assert page.locator('[data-leaving]').count() == 0
            assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
        assert page.locator('#blacklist').input_value() == 'scroll-draft.example.org'
        navigate('apps')
        assert page.locator('#appList').evaluate('e=>e.scrollTop') == app_y
        assert page.locator('#appList input').nth(7).is_checked()
        if reduced == 'no-preference':
            page.evaluate('''async () => {
              document.querySelector('nav [data-page=settings]').click();
              await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
            }''')
            page.set_viewport_size({'width': 744, 'height': 1133})
            settle()
            assert page.locator('.page:visible').count() == 1
            assert page.locator('[data-leaving]').count() == 0
            assert page.locator('.page').evaluate_all('els=>els.every(e=>!e.style.transform && !e.style.top)')
            page.set_viewport_size({'width': 393, 'height': 852})
            page.evaluate("document.querySelector('nav [data-page=rules]').click()")
            page.emulate_media(reduced_motion='reduce')
            settle()
            assert page.locator('.page:visible').count() == 1
            assert page.locator('[data-leaving]').count() == 0
            assert page.locator('#rules').evaluate('e=>!e.style.transform')
        assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|update-start|sources-save|apps-apply)(?: |$)/.test(c))")
        page.close()


def settings_and_status_regression(browser, errors, screenshots):
    for width, height in ((320, 740), (393, 852), (744, 900), (1180, 820)):
        for theme in ('light', 'dark'):
            page = browser.new_page(viewport={'width': width, 'height': height}, color_scheme=theme)
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.add_init_script(mock_script())
            page.goto((ROOT / 'module/webroot/index.html').as_uri())

            def back():
                # Locator.click scrolls sticky elements to their original flow
                # position before clicking. Real pointer taps must not do that.
                page.wait_for_function("!document.querySelector('main').classList.contains('sliding')")
                box = page.locator('#settingsBack').bounding_box()
                assert box and 0 <= box['y'] < height
                page.mouse.click(box['x'] + box['width'] / 2, box['y'] + box['height'] / 2)
                expect(page.locator('#settingsMenu')).to_be_visible()

            expect(page.locator('#startBtn')).to_be_enabled()
            for text in ('让连接更纯粹', '本地引擎', '无需 VPN', '规则自主', '安心，始于',
                         '按应用选择，让守护', '每一条规则，都由你', '了解你的模块', '更安静的网络'):
                assert text not in page.locator('body').text_content(), text
            assert page.locator('.page-heading .eyebrow, .page-heading .subtle').count() == 0
            assert page.locator('.guard-orbit').count() == 0
            assert page.locator('.guard-halo').count() == 0
            idle_core = page.locator('.guard-core').evaluate('e=>getComputedStyle(e).backgroundColor')
            idle_shield = page.locator('.guard-core path').get_attribute('d')
            page.evaluate('previewStartFails=false')
            page.locator('#startBtn').click()
            expect(page.locator('#stateTitle')).to_have_text('拦截已开启')
            halo = page.locator('.guard-halo')
            expect(halo).to_have_count(1)
            assert halo.evaluate('e=>getComputedStyle(e).animationName') == 'guard-breathe'
            assert page.locator('.guard-core').evaluate('e=>getComputedStyle(e).backgroundColor') != idle_core
            assert page.locator('.guard-core path').get_attribute('d') != idle_shield
            assert page.locator('.guard-emblem').evaluate('e=>getComputedStyle(e).transform') == 'none'
            offsets = halo.evaluate('''async e=>{
              const values=[];
              for(let n=0;n<5;n++) {await new Promise(r=>requestAnimationFrame(r));values.push(getComputedStyle(e).transform);}
              return values;
            }''')
            assert len(set(offsets)) > 1, offsets
            if screenshots:
                page.screenshot(path=str(screenshots / f'running-status-{theme}-{width}.png'))
            page.locator('nav [data-page="settings"]').click()
            expect(page.locator('#settingsMenu')).to_be_visible()
            page.wait_for_function("!document.querySelector('main').classList.contains('sliding')")
            assert halo.evaluate('e=>getComputedStyle(e).animationPlayState') == 'paused'
            assert page.locator('button.settings-entry:visible').count() == 4
            assert not page.locator('#previewCharacters').is_visible()
            assert not page.locator('#aboutModule').is_visible()
            assert not page.locator('#releaseNotes').is_visible()
            if screenshots:
                page.screenshot(path=str(screenshots / f'settings-menu-{theme}-{width}.png'))
            page.locator('[data-settings-entry="preview"]').focus()
            page.keyboard.press('Enter')
            expect(page.locator('.settings-heading h1')).to_be_focused()
            assert page.locator('.settings-heading h1').evaluate('e=>parseFloat(getComputedStyle(e).fontSize)') <= 16
            assert page.locator('#settingsBack').inner_text() == '设置'
            assert page.locator('.settings-heading').evaluate('e=>getComputedStyle(e).backgroundColor') == 'rgba(0, 0, 0, 0)'
            page.locator('#previewCharacters').fill('850')
            back()
            expect(page.locator('[data-settings-entry="preview"]')).to_be_focused()
            page.locator('[data-settings-entry="preview"]').click()
            expect(page.locator('#previewCharacters')).to_have_value('850')
            page.locator('#savePreviewSettings').click()
            page.keyboard.press('Escape')
            expect(page.locator('[data-settings-entry="preview"]')).to_contain_text('850 字符')
            page.locator('[data-settings-entry="releases"]').click()
            expect(page.locator('#releaseNotes')).to_be_visible()
            expect(page.locator('.release-list > li')).to_have_count(1)
            assert page.locator('.release-heading > b').all_text_contents() == ['v9.0']
            page.locator('#settings').evaluate('e=>e.scrollTop=130')
            saved = page.locator('#settings').evaluate('e=>e.scrollTop')
            page.locator('nav [data-page="apps"]').click()
            page.locator('nav [data-page="settings"]').click()
            expect(page.locator('#releaseNotes')).to_be_visible()
            assert abs(page.locator('#settings').evaluate('e=>e.scrollTop') - saved) <= 1, (width, theme, saved, 'tab return')
            back()
            page.locator('[data-settings-entry="about"]').click()
            expect(page.locator('#aboutVersion')).to_contain_text(VERSION_FIXTURE['version'])
            back()
            page.locator('[data-settings-entry="releases"]').click()
            assert abs(page.locator('#settings').evaluate('e=>e.scrollTop') - saved) <= 1, (width, theme, saved, 'detail return')
            page.set_viewport_size({'width': 393, 'height': 852})
            expect(page.locator('#releaseNotes')).to_be_visible()
            if screenshots:
                page.screenshot(path=str(screenshots / f'settings-detail-{theme}-{width}.png'))
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
            page.emulate_media(reduced_motion='reduce')
            page.locator('nav [data-page="home"]').click()
            assert halo.evaluate('e=>getComputedStyle(e).animationName') == 'none'
            assert page.locator('.hero').get_attribute('data-state') == 'ok'
            if screenshots:
                page.screenshot(path=str(screenshots / f'running-static-{theme}-{width}.png'))
            page.locator('#stopBtn').click()
            expect(page.locator('.guard-halo')).to_have_count(0)
            assert page.locator('.guard-core path').get_attribute('d') == idle_shield
            page.evaluate('previewStartFails=true')
            page.locator('#startBtn').click()
            expect(page.locator('#stateBadge')).to_have_text('上次启动失败')
            assert page.locator('.guard-halo').count() == 0
            page.close()


def status_card_regression(browser, errors, screenshots=None):
    """Compact hierarchy, full numeric values and safe actions in both themes."""
    for width in (280, 320, 393, 744, 1180):
        for theme in ('light', 'dark'):
            page = browser.new_page(viewport={'width': width, 'height': 852},
                                    color_scheme=theme, reduced_motion='reduce')
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.add_init_script(mock_script() + "\npreviewEngineState='running';previewStartFails=false;")
            page.goto((ROOT / 'module/webroot/index.html').as_uri())
            expect(page.locator('#blockedTotal')).to_have_text('154')
            expect(page.locator('#startBtn')).to_be_disabled()
            expect(page.locator('#stopBtn')).to_be_enabled()
            assert 'primary' in page.locator('#stopBtn').get_attribute('class').split()
            assert 'primary' not in page.locator('#startBtn').get_attribute('class').split()
            assert page.locator('.hero').bounding_box()['height'] < 355, (width, theme)
            for selector in ('#startLabel', '#stopLabel'):
                assert page.locator(selector).bounding_box()['height'] < 25, (width, theme, selector)
            assert page.locator('.guard-emblem').bounding_box()['width'] <= 56
            if screenshots and width in (320, 393, 1180):
                page.screenshot(path=str(screenshots / f'status-card-{theme}-{width}.png'))
            for count in (0, 1234567, 999999999, 1234567890, 9007199254740991, None):
                page.evaluate('''count=>{
                    previewMetrics={...previewMetrics,block_suffix_hits:count,block_exact_hits:0};
                }''', count)
                # Leaving/re-entering the overview starts a fresh read-only poll.
                page.locator('nav [data-page="apps"]').click()
                page.locator('nav [data-page="home"]').click()
                expected = '—' if count is None else f'{count:,}'
                expect(page.locator('#blockedTotal')).to_have_text(expected)
                if count is None:
                    expect(page.locator('.blocked-summary small')).to_have_text('统计内容未能核实')
                    expect(page.locator('.blocked-unit')).to_have_count(0)
                else:
                    expect(page.locator('.blocked-unit')).to_have_text('次')
                for selector in ('.hero', '.hero-heading', '.blocked-summary', '.blocked-value', '#blockedTotal'):
                    assert page.locator(selector).evaluate('e=>e.scrollWidth<=e.clientWidth+1'), (width, theme, count, selector)
                assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
            if width == 320 and theme == 'light':
                page.evaluate("previewMetrics={...previewMetrics,block_suffix_hits:7,age_ms:44900}")
                page.locator('nav [data-page="apps"]').click()
                page.locator('nav [data-page="home"]').click()
                expect(page.locator('#blockedTotal')).to_have_text('7')
                expect(page.locator('.blocked-summary small')).to_have_text('统计已过期，保留上次记录', timeout=3000)
                expect(page.locator('.blocked-summary small')).to_have_attribute('data-stale','true')
                page.evaluate("previewMetrics={...previewMetrics,status:'unverified',owner_verified:false}")
                page.locator('nav [data-page="apps"]').click()
                page.locator('nav [data-page="home"]').click()
                expect(page.locator('#blockedTotal')).to_have_text('—')
                expect(page.locator('.blocked-summary small')).to_contain_text('归属未核实')
            assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|autostart)(?: |$)/.test(c))")
            page.locator('#stopBtn').click()
            expect(page.locator('.blocked-summary')).to_have_count(0)
            expect(page.locator('#stateTitle')).to_have_text('当前未接管网络')
            assert 'primary' in page.locator('#startBtn').get_attribute('class').split()
            page.locator('#startBtn').click()
            expect(page.locator('#stateTitle')).to_have_text('拦截已开启')
            page.close()


def version_fallback_regression(browser, errors):
    # Missing bridge still allows version fallback and local settings; a valid
    # but mismatched binary must not be disguised as the bundled version.
    for kind in ('offline', 'mismatch'):
        page = browser.new_page(viewport={'width': 320, 'height': 740}, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        if kind == 'mismatch':
            page.add_init_script(mock_script() + '\npreviewVersion={module:"AD_Killer_By_NFQUEUE",version:"1.0.0",versionCode:1};')
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#moduleVersion')).to_contain_text('v' + (VERSION_FIXTURE['version'] if kind == 'offline' else '1.0.0'))
        page.locator('nav [data-page="settings"]').click()
        page.locator('[data-settings-entry="about"]').click()
        assert page.locator('#downloadModuleUpdate').count() == 0
        if kind == 'offline':
            expect(page.locator('#moduleUnverified')).to_contain_text('尚未核实')
        else:
            expect(page.locator('#versionMismatch')).to_be_visible()
        page.locator('#settingsBack').click()
        page.locator('[data-settings-entry="updates"]').click()
        expect(page.locator('#getUpdate')).to_be_enabled()
        page.close()


def module_update_regression(browser, errors, screenshots=None):
    for width in (320, 393):
        page = browser.new_page(viewport={'width': width, 'height': 852}, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script())
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#moduleVersion')).to_contain_text('v' + VERSION_FIXTURE['version'])
        page.locator('nav [data-page="settings"]').click()
        page.locator('[data-settings-entry="about"]').click()
        assert page.locator('.about-identity h3').inner_text() == 'AD Killer'
        assert page.locator('#aboutModule').get_by_text('版本来源',exact=True).count() == 0
        assert page.locator('#aboutModule').get_by_text('界面版本',exact=True).count() == 0
        author=page.locator('.module-facts a[href="https://github.com/fengwenhua"]')
        assert author.get_attribute('href') == 'https://github.com/fengwenhua'
        assert author.inner_text().strip() == 'fengwenhua'
        facts=page.locator('.module-facts > div')
        assert facts.locator('dt').all_text_contents() == ['模块版本', '作者', '致谢', '数据目录']
        thanks=facts.filter(has=page.locator('dt', has_text='致谢')).get_by_role('link')
        assert thanks.inner_text().strip() == '感谢 YH_Hosts'
        assert thanks.get_attribute('href') == 'https://yhyun.asia/'
        assert thanks.get_attribute('target') == '_blank'
        assert set(thanks.get_attribute('rel').split()) >= {'noopener', 'noreferrer'}
        assert thanks.bounding_box()['height'] >= 44
        github=page.locator('.github-link')
        assert github.get_attribute('href') == 'https://github.com/fengwenhua/AD_Killer_By_NFQUEUE'
        expect(github).to_contain_text('在GitHub上查看')
        assert github.bounding_box()['height'] >= 76
        if screenshots:
            page.screenshot(path=str(screenshots / f'about-v12-{width}.png'))
        page.locator('#settingsBack').click()
        assert page.locator('#autoCheckUpdate').is_checked() is False
        auto=page.locator('.settings-auto')
        assert auto.locator('.settings-entry-icon').count() == 1
        labels=page.locator('#settingsMenu .settings-entry-label, #settingsMenu .switch-copy label').all()
        assert max(label.bounding_box()['x'] for label in labels)-min(label.bounding_box()['x'] for label in labels) <= 1
        assert page.locator('#settingsMenu').evaluate("e=>Array.from(e.children).map(c=>c.classList.contains('settings-auto')?'auto':c.dataset.settingsEntry).join(',')") == 'preview,auto,updates,releases,about'
        page.locator('[data-settings-entry="updates"]').click()
        assert not page.evaluate("previewCommands.some(c=>c.includes('--module-update-check'))")
        switch=page.locator('#updatePreview')
        assert switch.bounding_box()['width'] == 48
        assert switch.bounding_box()['height'] == 44
        switch.check()
        assert switch.evaluate("e=>getComputedStyle(e,'::after').transform") == 'matrix(1, 0, 0, 1, 18, 0)'
        assert page.locator('#settings-updates details, #settings-updates a').count() == 0
        if screenshots:
            page.screenshot(path=str(screenshots / f'updates-v12-{width}.png'))
        page.evaluate('previewModuleCheckDelay=700')
        page.locator('#getUpdate').click()
        expect(page.locator('#getUpdate')).to_have_attribute('aria-busy','true')
        assert page.locator('#getUpdate').is_disabled()
        assert 'loading' in page.locator('#getUpdate').get_attribute('class')
        expect(page.locator('#getUpdate')).to_contain_text('检查中')
        expect(page.locator('#downloadModuleUpdate')).to_be_visible()
        expect(page.locator('#getUpdate')).to_have_attribute('aria-busy','false')
        assert not page.evaluate("previewCommands.some(c=>c.includes('--module-update-download'))")
        page.locator('#downloadModuleUpdate').click()
        expect(page.locator('#installModuleUpdate')).to_be_visible()
        assert not page.evaluate("previewCommands.some(c=>c.includes('--module-update-install'))")
        page.locator('#installModuleUpdate').click()
        expect(page.locator('#confirmModuleInstall')).to_be_visible()
        assert not page.evaluate("previewCommands.some(c=>c.includes('--module-update-install'))")
        page.locator('#confirmModuleInstall').click()
        expect(page.locator('#moduleUpdateStatus')).to_contain_text('安装成功')
        assert page.locator('#installModuleUpdate').count() == 0
        page.locator('#settingsBack').click()
        page.evaluate('previewReleaseNotesFails=true')
        page.locator('[data-settings-entry="releases"]').click()
        expect(page.locator('.release-error')).to_be_visible()
        assert page.locator('.release-list > li').count() == 0
        page.evaluate('previewReleaseNotesFails=false')
        page.locator('#reloadReleaseNotes').click()
        expect(page.locator('#reloadReleaseNotes')).to_have_attribute('aria-busy','true')
        expect(page.locator('#reloadReleaseNotes')).to_be_disabled()
        expect(page.locator('#reloadReleaseNotes')).to_have_text('加载中…')
        expect(page.locator('.release-heading > b')).to_have_text('v9.0')
        expect(page.locator('.release-body')).to_contain_text('来自 GitHub Release')
        assert page.locator('.release-list > li').count() == 1
        assert page.locator('.release-body img').count() == 0
        assert page.evaluate('previewNativeBusy.every(item=>item.busy)'), 'native bridge was called before busy DOM rendered'
        if screenshots:
            page.screenshot(path=str(screenshots / f'release-notes-v12-{width}.png'))
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
        page.close()

    page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script())
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#moduleVersion')).to_contain_text('v' + VERSION_FIXTURE['version'])
    page.locator('nav [data-page="settings"]').click()
    page.locator('#autoCheckUpdate').check()
    page.wait_for_function("previewCommands.some(c=>c.includes('--module-update-check stable'))")
    page.reload()
    page.wait_for_function("previewCommands.some(c=>c.includes('--module-update-check stable'))")
    assert page.evaluate("previewCommands.filter(c=>c.includes('--module-update-check')).length") == 1
    assert not page.evaluate("previewCommands.some(c=>/--module-update-(download|install)/.test(c))")
    page.locator('nav [data-page="settings"]').click()
    expect(page.locator('#autoCheckUpdate')).to_be_checked()
    page.locator('#autoCheckUpdate').uncheck()
    page.reload()
    expect(page.locator('#appList')).to_have_attribute('aria-busy','false')
    assert not page.evaluate("previewCommands.some(c=>c.includes('--module-update-check'))")
    page.close()


def update_failure_feedback_regression(browser, errors, screenshots=None):
    # Real failure mode: an immediate cached/API 403, not an artificial 700ms
    # request. Native entry must already see busy DOM, with stable retry focus.
    for theme in ('light', 'dark'):
        page = browser.new_page(viewport={'width': 320, 'height': 852}, color_scheme=theme, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script() + '\npreviewModuleCheckFails=true;previewReleaseNotesFails=true;previewRulesEmpty=true;')
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#moduleVersion')).to_contain_text('v' + VERSION_FIXTURE['version'])
        page.locator('#startBtn').click()
        expect(page.locator('#stateDesc')).to_contain_text('请先点击“同步开源规则”')
        assert page.evaluate('previewEngineState') == 'stopped'
        assert not page.evaluate("previewCommands.some(c=>c.includes('--status-fast'))"), 'empty-rule failure must not poll for a nonexistent daemon'
        page.locator('nav [data-page="settings"]').click()
        if screenshots:
            page.screenshot(path=str(screenshots / f'settings-auto-{theme}-320.png'))
        page.locator('[data-settings-entry="updates"]').click()
        page.locator('#getUpdate').click()
        expect(page.locator('#getUpdate')).to_have_attribute('aria-busy', 'true')
        expect(page.locator('#getUpdate')).to_be_disabled()
        assert page.locator('#getUpdate').evaluate("e=>getComputedStyle(e,'::before').content") != 'none'
        if screenshots:
            page.screenshot(path=str(screenshots / f'update-check-loading-{theme}-320.png'))
        expect(page.locator('#getUpdate')).to_have_attribute('aria-busy', 'false')
        expect(page.locator('#moduleUpdateStatus')).to_contain_text('HTTP 403')
        expect(page.locator('#moduleUpdateStatus')).to_contain_text('匿名请求配额')
        assert page.locator('#downloadModuleUpdate').count() == 0
        page.locator('#settingsBack').click()
        page.locator('[data-settings-entry="releases"]').click()
        expect(page.locator('.release-error')).to_be_visible()
        retry = page.locator('#reloadReleaseNotes')
        retry.click()
        expect(retry).to_have_attribute('aria-busy', 'true')
        expect(retry).to_have_text('加载中…')
        expect(retry).to_be_disabled()
        retry.evaluate('e=>e.blur()')  # Also exercise browsers that blur on disable.
        if screenshots:
            page.screenshot(path=str(screenshots / f'release-retry-loading-{theme}-320.png'))
        expect(retry).to_have_attribute('aria-busy', 'false')
        expect(retry).to_be_focused()
        expect(page.locator('.release-error')).to_contain_text('HTTP 403')
        retry.click()
        expect(retry).to_have_attribute('aria-busy', 'true')
        page.locator('#settingsBack').focus()
        expect(retry).to_have_attribute('aria-busy', 'false')
        expect(page.locator('#settingsBack')).to_be_focused()
        assert page.evaluate('previewNativeBusy.every(item=>item.busy)')
        assert page.evaluate('document.documentElement.scrollWidth<=innerWidth+1')
        page.close()


def lazy_sources_and_denied_settings_regression(browser, errors):
    page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script([{'url': f'https://example.org/source{i}', 'enabled': True} for i in range(64)]))
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('.source-item')).to_have_count(64)
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    assert page.locator('.source-card-content').count() < 24
    last = page.locator('.source-item').last
    last.scroll_into_view_if_needed()
    expect(last.locator('input')).to_be_visible()
    page.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))')
    last.locator('input').uncheck()
    last.locator('input').focus()
    top = last.bounding_box()['y']
    page.keyboard.press('Space')
    expect(page.locator('.source-card-content')).to_have_count(64)
    assert last.locator('input').is_checked()
    assert abs(last.bounding_box()['y'] - top) <= 1, 'keyboard eager rendering lost viewport anchor'
    saved_scroll = page.locator('#rules').evaluate('e=>e.scrollTop')
    page.locator('nav [data-page="home"]').click()
    page.locator('nav [data-page="rules"]').click()
    assert abs(page.locator('#rules').evaluate('e=>e.scrollTop') - saved_scroll) <= 1
    last.get_by_role('button', name='查看内容', exact=True).click()
    expect(page.locator('dialog pre')).to_be_visible()
    page.keyboard.press('Escape')
    assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon|reload|update-start|sources-save|source-test)(?: |$)/.test(c))")
    page.close()

    page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script("Object.defineProperty(window,'localStorage',{get(){throw new Error('denied')}});\n" + mock_script())
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('.app')).to_have_count(2)
    page.locator('nav [data-page="settings"]').click()
    page.locator('[data-settings-entry="preview"]').click()
    expect(page.locator('#previewCharacters')).to_have_value('500')
    page.locator('#previewCharacters').fill('700')
    page.locator('#savePreviewSettings').click()
    expect(page.locator('#previewSettingsStatus')).to_contain_text('无法持久保存')
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    page.locator('.source-item').first.get_by_role('button', name='查看内容', exact=True).click()
    expect(page.locator('dialog pre')).to_be_visible()
    assert len(page.locator('dialog pre').inner_text()) == 700
    page.close()


def virtual_apps_regression(browser, errors):
    page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion='reduce')
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script() + r'''
      ksu.listPackages=()=>JSON.stringify(Array.from({length:800},(_,i)=>`org.example.app${String(i).padStart(3,'0')}`));
      ksu.getPackagesInfo=p=>JSON.stringify(JSON.parse(p).map(packageName=>({packageName,appLabel:packageName})));
    ''')
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    expect(page.locator('#appList')).to_have_attribute('data-total', '800')
    page.locator('nav [data-page="apps"]').click()
    assert page.locator('.app').count() < 40
    page.locator('[data-app-index="0"]').focus()
    for _ in range(22):
        page.keyboard.press('Tab')
    assert page.evaluate('document.activeElement.dataset.appIndex') == '22'
    page.keyboard.press('Shift+Tab')
    assert page.evaluate('document.activeElement.dataset.appIndex') == '21'
    page.keyboard.press('Space')
    assert page.locator('[data-app-index="21"]').is_checked()
    page.locator('#appList').evaluate("e=>{e.scrollTop=e.scrollHeight;e.dispatchEvent(new Event('scroll'))}")
    expect(page.locator('[data-app-index="799"]')).to_be_attached()
    page.locator('[data-app-index="799"]').check()
    saved_scroll = page.locator('#appList').evaluate('e=>e.scrollTop')
    page.locator('nav [data-page="home"]').click()
    page.locator('nav [data-page="apps"]').click()
    assert page.locator('#appList').evaluate('e=>e.scrollTop') == saved_scroll
    assert page.locator('[data-app-index="799"]').is_checked()
    # At the bottom, changing from one to two columns must not leave an empty
    # range whose oversized spacer prevents the browser from clamping scroll.
    page.set_viewport_size({'width': 744, 'height': 1133})
    expect(page.locator('[data-app-index="799"]')).to_be_attached()
    assert 0 < page.locator('.app').count() < 40
    page.locator('#appSearch').fill('org.example.app799')
    expect(page.locator('.app')).to_have_count(1)
    assert page.locator('.app input').is_checked()
    assert page.locator('#appList').evaluate('e=>e.scrollTop') == 0
    page.locator('#appSearch').fill('no-such-application')
    expect(page.locator('#appList .empty-state')).to_be_visible()
    page.locator('#appSearch').fill('')
    expect(page.locator('#appList')).to_have_attribute('data-total', '800')
    page.locator('#appList').evaluate("e=>{e.scrollTop=21*80/2;e.dispatchEvent(new Event('scroll'))}")
    expect(page.locator('[data-app-index="21"]')).to_be_attached()
    assert page.locator('[data-app-index="21"]').is_checked()
    page.close()


ADAPTIVE_VIEWPORTS = (
    ('phone-small', 320, 740), ('phone', 393, 852), ('phone-large', 430, 932),
    ('large-fold-cover', 351, 852), ('large-fold-open', 744, 1133),
    ('large-fold-square', 840, 1080), ('large-fold-rotated', 1133, 744),
    ('wide-fold-portrait', 480, 600), ('wide-fold-short', 600, 480),
    ('wide-fold-landscape', 720, 540), ('tablet-small', 768, 1024),
    ('tablet', 820, 1180), ('tablet-large', 1024, 1366),
    ('tablet-landscape', 1180, 820), ('tablet-large-landscape', 1366, 1024),
    ('phone-landscape', 852, 393),
)


def adaptive_regression(browser, screenshots, errors):
    """Exercise the compiled artifact offline, with no app internals exported."""
    def compact_header(page, name, theme):
        assert page.locator('.appbar').bounding_box()['height'] <= 60, (name, theme, 'oversized header')
        brand=page.locator('.brand-copy')
        assert brand.locator('b').inner_text() == 'AD Killer'
        assert brand.locator('.brand-subtitle').inner_text() == 'by NFQUEUE'
        assert brand.evaluate('e=>e.scrollWidth <= e.clientWidth'), (name, theme, 'brand overflow')
        title_y=brand.locator('b').bounding_box()['y']
        subtitle_y=brand.locator('.brand-subtitle').bounding_box()['y']
        assert title_y <= subtitle_y < title_y + 10, (name, theme, 'brand must stay on one row')
        button=page.locator('.theme-toggle').bounding_box()
        assert button['height'] >= 44 and button['width'] >= 44

    for theme in ('light', 'dark'):
        # Check the header even below the full-page 320px regression baseline;
        # other page controls may legitimately wrap at this exceptional width.
        page=browser.new_page(viewport={'width': 280, 'height': 653}, color_scheme=theme)
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script())
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('.brand-copy b')).to_be_visible()
        compact_header(page, 'minimum-header', theme)
        page.close()
        for name, width, height in ADAPTIVE_VIEWPORTS:
            page = browser.new_page(viewport={'width': width, 'height': height},
                                    color_scheme=theme, reduced_motion='reduce')
            requests = []
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.on('request', lambda request: requests.append(request.url))
            page.add_init_script(mock_script())
            page.goto((ROOT / 'module/webroot/index.html').as_uri())
            page.wait_for_function("document.querySelector('#ruleTotal')?.textContent === '186,757'")
            compact_header(page, name, theme)
            for selector in ('#ruleTotal', '#startLabel', '#stopLabel'):
                assert page.locator(selector).evaluate('e=>e.getBoundingClientRect().height <= parseFloat(getComputedStyle(e).lineHeight) + 1'), (name, selector, 'text wrapped')
            for tab in ('home', 'apps', 'rules', 'settings'):
                page.locator(f'nav [data-page="{tab}"]').click()
                if tab == 'rules':
                    page.locator('#sourcesDetails > summary').click()
                    page.locator('#customRulesDetails > summary').click()
                assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1'), (name, theme, tab)
                for control in page.locator('button:visible, input[type=checkbox]:visible, summary:visible').all():
                    box = control.bounding_box()
                    assert box and box['height'] >= 44 and box['width'] >= 44, (name, tab, box)
                    assert box['x'] >= -1 and box['x'] + box['width'] <= width + 1, (name, tab, box)
                if screenshots and name in ('phone', 'large-fold-open', 'wide-fold-landscape', 'tablet-landscape'):
                    # Home/apps are viewport captures; source list intentionally scrolls.
                    page.screenshot(path=str(screenshots / f'adaptive-{name}-{theme}-{tab}.png'), full_page=True)
            rail = width >= 960 or (width >= 600 and height <= 600 and width > height)
            assert page.locator('nav').evaluate('e=>getComputedStyle(e).flexDirection') == ('column' if rail else 'row'), name
            assert page.locator('.hero').evaluate("e=>getComputedStyle(e,'::after').animationName") == 'none'
            assert not any(url.startswith(('http:', 'https:')) for url in requests), requests
            page.close()

    # A real resize sequence, not reloads or UA detection. Drafts, details,
    # selection, focused input, active route and observers must survive.
    page = browser.new_page(viewport={'width': 393, 'height': 852})
    page.add_init_script(mock_script())
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.goto((ROOT / 'module/webroot/index.html').as_uri())
    page.wait_for_function("document.querySelectorAll('.app').length === 2")
    page.locator('nav [data-page="apps"]').click()
    page.locator('#appList input').first.check()
    page.locator('#appSearch').fill('浏览器')
    page.locator('nav [data-page="rules"]').click()
    page.locator('#sourcesDetails > summary').click()
    page.locator('#customRulesDetails > summary').click()
    page.locator('#blacklist').fill('draft.example.org\n||long-domain.example.org^')
    page.locator('#source-0').uncheck()
    page.locator('#addSource').click()
    page.locator('#sourceUrl').fill('https://example.org/unsaved')
    for _, width, height in (ADAPTIVE_VIEWPORTS[4], ADAPTIVE_VIEWPORTS[8], ADAPTIVE_VIEWPORTS[9], ADAPTIVE_VIEWPORTS[13], ADAPTIVE_VIEWPORTS[10], ADAPTIVE_VIEWPORTS[1]):
        page.set_viewport_size({'width': width, 'height': height})
        assert page.locator('nav [aria-current="page"]').get_attribute('data-page') == 'rules'
        assert page.locator('#sourceUrl').input_value() == 'https://example.org/unsaved'
        assert page.locator('#sourceUrl').evaluate('e=>document.activeElement===e')
        assert page.locator('#sourcesDetails').evaluate('e=>e.open')
        assert page.locator('#blacklist').input_value().startswith('draft.example.org')
        assert not page.locator('#source-0').is_checked()
        assert not page.locator('#saveSources').is_disabled()
        assert page.evaluate('document.documentElement.scrollWidth <= innerWidth + 1')
        assert page.evaluate("previewCommands.filter(c=>c.includes(' --sources-list')).length") == 1
    page.keyboard.press('Escape')
    expect(page.locator('#sourceEditorDialog')).to_have_count(0)
    page.locator('nav [data-page="apps"]').click()
    assert page.locator('#appSearch').input_value() == '浏览器'
    assert '1 个放行' in page.locator('#appCount').inner_text()
    page.locator('#saveApps').click()
    page.wait_for_function("previewCommands.some(c=>/ --apps-apply (?:allow|intercept) [a-f0-9]{64}$/.test(c))")
    page.locator('nav [data-page="rules"]').click()
    page.locator('#saveRules').click()
    page.wait_for_function("previewCommands.some(c=>/ --reload custom [a-f0-9]{64}$/.test(c))")
    assert page.evaluate("previewCommands.some(c=>c.includes('base64 -d |') && c.includes('--config-save custom') && atob(c.match(/'([A-Za-z0-9+/=]+)'/)[1]).includes('draft.example.org'))")
    page.locator('nav [data-page="home"]').click()
    # Persisting boot policy must never invoke engine controls.
    page.locator('#autostart').check()
    page.wait_for_function("previewCommands.some(c=>c.endsWith(' --autostart on')) && !document.querySelector('#autostart').matches(':disabled')")
    assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon)$/.test(c))")
    page.locator('#autostart').uncheck()
    page.wait_for_function("!document.querySelector('#autostart').matches(':disabled')")
    page.locator('#scopeSeg [data-scope="1"]').click()
    page.wait_for_function("document.querySelector('#appTitle').textContent === '应用拦截名单'")
    page.evaluate('previewStartDelay=200')
    page.locator('#startBtn').click()
    assert page.locator('#startBtn').get_attribute('aria-busy') == 'true'
    assert page.locator('#startBtn').is_disabled() and page.locator('#stopBtn').is_disabled()
    page.wait_for_function("document.querySelector('#stateBadge').textContent === '上次启动失败'")
    assert page.locator('#startBtn').get_attribute('aria-busy') == 'false'
    assert 'YH_hosts' in page.locator('#stateDesc').inner_text()
    page.close()


def quic_setting_regression(browser, errors):
    # Both fresh-install ON and an existing user's OFF must reflect the native
    # configuration; the UI must never opt users back in on mount/refresh.
    for enabled in (True, False):
        page=browser.new_page(viewport={'width': 320, 'height': 740}, reduced_motion='reduce')
        page.on('pageerror', lambda error: errors.append(str(error)))
        page.add_init_script(mock_script() + f'\npreviewSettings.block_quic={json.dumps(enabled)};')
        page.goto((ROOT / 'module/webroot/index.html').as_uri())
        expect(page.locator('#quic')).to_be_checked(checked=enabled)
        assert not page.evaluate("previewCommands.some(c=>c.includes(' --quic '))")
        page.locator('#quic').set_checked(not enabled)
        expect(page.locator('#quic')).to_be_checked(checked=not enabled)
        page.wait_for_function('expected=>previewSettings.block_quic===expected', arg=not enabled)
        page.locator('nav [data-page="settings"]').click()
        page.locator('nav [data-page="home"]').click()
        expect(page.locator('#quic')).to_be_checked(checked=not enabled)
        page.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--browser-path")
    parser.add_argument("--screenshots", type=Path)
    args = parser.parse_args()
    if args.screenshots:
        args.screenshots.mkdir(parents=True, exist_ok=True)
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(executable_path=args.browser_path)
        print(f'WebUI regression: Playwright {version("playwright")}, Chromium {browser.version}', flush=True)
        errors = []
        try:
            fixture_regression(browser, errors)
            freezer_health_regression(browser, errors)
            cleanup_health_regression(browser, errors)
            apply_health_regression(browser, errors)
            background_job_recovery_regression(browser, errors)
            quic_setting_regression(browser, errors)
            preview_regression(browser, errors, args.screenshots)
            source_editor_dialog_regression(browser, errors, args.screenshots)
            motion_and_settings_regression(browser, errors, args.screenshots)
            scroll_and_push_regression(browser, errors)
            settings_and_status_regression(browser, errors, args.screenshots)
            status_card_regression(browser, errors, args.screenshots)
            version_fallback_regression(browser, errors)
            lazy_sources_and_denied_settings_regression(browser, errors)
            virtual_apps_regression(browser, errors)
            for theme in ("light", "dark"):
                for width in (320, 351, 360, 393, 430, 640, 760, 768, 912, 1024):
                    page = browser.new_page(viewport={"width": width, "height": 852},
                                            device_scale_factor=1, color_scheme=theme, reduced_motion='reduce')
                    page.on("pageerror", lambda error: errors.append(str(error)))
                    page.add_init_script(mock_script())
                    page.goto((ROOT / "module/webroot/index.html").as_uri())
                    page.wait_for_function("document.querySelector('#ruleTotal')?.textContent !== undefined && document.querySelector('#ruleTotal').textContent !== '—'")
                    page.wait_for_function("document.querySelector('#appCount')?.textContent === '0 个放行'")
                    assert page.evaluate("document.documentElement.dataset.theme") == theme
                    colors = page.evaluate("Object.fromEntries(['muted','surface','text','bg','main','on-main','bad','ok','main-soft','warn','warn-soft'].map(k=>[k,getComputedStyle(document.documentElement).getPropertyValue('--'+k).trim()]))")
                    for foreground, background in (("muted", "surface"), ("text", "bg"),
                                                   ("on-main", "main"), ("bad", "surface"),
                                                   ("ok", "main-soft"), ("warn", "surface"),
                                                   ("text", "warn-soft")):
                        levels = sorted((luminance(colors[foreground]), luminance(colors[background])))
                        assert (levels[1] + 0.05) / (levels[0] + 0.05) >= 4.5, (theme, foreground)
                    assert page.evaluate("new Set([...document.querySelectorAll('[id]')].map(e=>e.id)).size === document.querySelectorAll('[id]').length")
                    for tab in ("home", "apps", "rules", "settings"):
                        page.locator(f'nav button[data-page="{tab}"]').click()
                        assert page.locator(f"#{tab}").is_visible(), (width, theme, tab)
                        assert page.locator(f"#{tab}").evaluate("e=>e.parentElement.tagName") == "MAIN"
                        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth + 1"), (width, theme, tab)
                        assert page.locator('nav [aria-current="page"]').get_attribute("data-page") == tab
                        if tab == "rules":
                            for selector in ("#sourcesDetails", "#customRulesDetails"):
                                assert not page.locator(selector).evaluate("e=>e.open"), "rules must default collapsed"
                            assert not page.locator("#sourceList").is_visible()
                            assert not page.locator("#blacklist").is_visible()
                            if args.screenshots and width == 393:
                                page.screenshot(path=str(args.screenshots / f"rules-collapsed-{theme}-393.png"), full_page=True)
                            page.locator("#sourcesDetails > summary").focus()
                            page.keyboard.press("Enter")
                            assert page.locator("#sourcesDetails").evaluate("e=>e.open")
                            assert not page.locator("#customRulesDetails").evaluate("e=>e.open")
                            page.locator("#customRulesDetails > summary").focus()
                            page.keyboard.press("Space")
                            assert page.locator("#blacklist").is_visible()
                            page.locator("#sourcesDetails > summary").click()
                            assert not page.locator("#sourceList").is_visible()
                            assert page.locator("#blacklist").is_visible(), "folds must be independent"
                            page.locator("#sourcesDetails > summary").click()
                            page.wait_for_function("document.querySelector('.source-item time') !== null")
                            assert "同步成功" in page.locator(".source-item").first.inner_text()
                            assert page.locator(".source-item time").first.get_attribute("datetime")
                        for element in page.locator("button:visible, input[type=checkbox]:visible, summary:visible").all():
                            box = element.bounding_box()
                            assert box and box["height"] >= 44 and box["width"] >= 44, (width, tab, box)
                        if args.screenshots and width == 393:
                            page.screenshot(path=str(args.screenshots / f"{tab}-{theme}-393.png"), full_page=True)
                    page.locator('nav [data-page="home"]').click()
                    # Successful and failed job feedback must both be visible
                    # without inferring the result from the button click alone.
                    if width == 393:
                        page.locator("#syncBtn").click()
                        page.wait_for_function("document.querySelector('#updateStatus').dataset.kind === 'success'")
                        assert "73,170" in page.locator("#updateStatus").inner_text()
                        page.evaluate("window.previewPartial=true")
                        page.locator("#syncBtn").click()
                        page.wait_for_function("document.querySelector('#updateStatus').dataset.kind === 'warning'")
                        assert "部分来源失败" in page.locator("#updateStatus").inner_text()
                        # Independently scrolled inactive panes are visibility-
                        # hidden, not display:none. Verify the history users
                        # actually see when returning, not hidden innerText.
                        page.locator('nav [data-page="rules"]').click()
                        expect(page.locator('.source-item').first).to_be_visible()
                        assert "同步失败" in page.locator(".source-item").first.inner_text()
                        assert "network_timeout" in page.locator(".source-item").first.inner_text()
                        assert page.locator(".source-item time").first.get_attribute("datetime")
                        page.locator('nav [data-page="home"]').click()
                        page.evaluate("window.previewUpdateFails=true")
                        page.locator("#syncBtn").click()
                        page.wait_for_function("document.querySelector('#updateStatus').dataset.kind === 'error'")
                        assert page.locator("#consoleDetails").evaluate("e=>e.open")
                        assert "staging" in page.locator("#updateStatus").inner_text()
                        # Actual UI source CRUD, disabled-source testing and
                        # explicit-save semantics; no daemon start commands.
                        page.locator('nav [data-page="rules"]').click()
                        page.locator("#addSource").click()
                        page.locator("#sourceUrl").fill("file:///etc/passwd")
                        page.get_by_role("button", name="确认地址").click()
                        assert page.locator("#sourceUrl").get_attribute("aria-invalid") == "true"
                        page.locator("#sourceUrl").fill("https://example.org/" + "long/" * 20 + "a'b?x=1&y=2")
                        page.get_by_role("button", name="确认地址").click()
                        assert page.locator(".source-item").count() == 4
                        assert page.evaluate("document.documentElement.scrollWidth <= innerWidth + 1")
                        assert page.evaluate("previewSources.length") == 3, "draft saved implicitly"
                        page.locator("#source-3").uncheck()
                        page.evaluate("previewSaveFails=true")
                        page.locator("#saveSources").click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('保存失败')")
                        assert page.evaluate("previewSources.length") == 3
                        assert not page.locator("#saveSources").is_disabled()
                        page.evaluate("previewSaveFails=false")
                        page.locator("#saveSources").click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('已保存')")
                        assert not page.evaluate("previewSources[3].enabled")
                        # Disabled sources sink to the bottom after save.
                        assert "已启用" in page.locator(".source-item").first.inner_text()
                        assert "已停用" in page.locator(".source-item").last.inner_text()
                        page.locator(".source-item").nth(3).get_by_role("button", name="测试", exact=True).click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').dataset.kind === 'success'")
                        assert "未替换规则" in page.locator("#sourceTestLog-3").inner_text()
                        assert page.evaluate("previewCommands.some(c=>c.includes(\" --source-test 'https://example.org/\") && c.includes(\"'\\\\''\"))")
                        page.evaluate("previewTestFails=true")
                        page.locator(".source-item").nth(3).get_by_role("button", name="测试", exact=True).click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').dataset.kind === 'error'")
                        assert "当前规则未改变" in page.locator("#sourcesStatus").inner_text()
                        page.locator(".source-item").nth(3).get_by_role("button", name="编辑", exact=True).click()
                        page.locator("#sourceUrl").fill("https://example.org/edited")
                        page.get_by_role("button", name="确认地址").click()
                        page.once("dialog", lambda dialog: dialog.accept())
                        page.locator(".source-item").nth(3).get_by_role("button", name="删除", exact=True).click()
                        assert page.locator(".source-item").count() == 3
                        page.locator("#saveSources").click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('已保存')")
                        special=page.locator('.source-item[data-builtin="true"]')
                        assert special.count() == 1 and "Pro − Fake" in special.inner_text()
                        assert special.get_by_role("button", name="编辑", exact=True).is_disabled()
                        assert special.get_by_role("button", name="删除", exact=True).is_disabled()
                        assert page.locator("#sourceEditor").is_hidden() and special.count() == 1
                        page.evaluate("previewTestFails=false")
                        special.get_by_role("button", name="测试", exact=True).click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').dataset.kind === 'success'")
                        assert page.evaluate("previewCommands.some(c=>c.includes(\" --source-test 'builtin:hagezi-pro-minus-fake'\"))")
                        for enabled in (True, False):
                            page.locator("#source-2").set_checked(enabled)
                            page.locator("#saveSources").click()
                            page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('已保存')")
                            assert page.evaluate("previewSources.find(s=>s.locked).enabled") is enabled
                        assert not page.evaluate("previewCommands.some(c=>/ --(?:start|stop|daemon)(?: |$)/.test(c))")
                        page.locator('nav [data-page="home"]').click()
                        page.locator("#startBtn").click()
                        page.wait_for_function("document.querySelector('#stateBadge').textContent === '上次启动失败'")
                        assert "当前未运行" in page.locator("#stateTitle").inner_text()
                        assert "YH_hosts" in page.locator("#stateDesc").inner_text()
                        assert "启动超时" not in page.locator("#console").inner_text()
                        assert page.locator("#consoleDetails").evaluate("e=>e.open")
                        page.evaluate("previewStartFails=false")
                        page.locator("#startBtn").click()
                        page.wait_for_function("document.querySelector('#stateBadge').textContent === '正在守护'")
                        page.locator("#stopBtn").click()
                        page.wait_for_function("document.querySelector('#stateBadge').textContent === '已暂停'")
                        assert not page.locator("#autostart").is_checked()
                        assert not page.evaluate("previewCommands.some(c=>c.includes('rm -f ')||c.includes('nohup '))")
                    if width in (320, 393):
                        # Draft switches keep their committed positions, keyboard
                        # focus and viewport anchor. Only successful save sorts.
                        page.close()
                        page = browser.new_page(viewport={"width": width, "height": 852},
                                                device_scale_factor=1, color_scheme=theme, reduced_motion='reduce')
                        page.on("pageerror", lambda error: errors.append(str(error)))
                        page.add_init_script(mock_script(ORDERING_SOURCES))
                        page.goto((ROOT / 'module/webroot/index.html').as_uri())
                        label = f'{theme} {width}px committed source-order fixture'
                        expect(page.locator('.source-item'), label).to_have_count(5)
                        page.reload()
                        expect(page.locator('.source-item'), label + ' after reload').to_have_count(5)
                        page.locator('nav [data-page="rules"]').click()
                        page.locator('#sourcesDetails > summary').click()

                        def source_order():
                            return page.locator('#sourceList input[type="checkbox"]').evaluate_all("els=>els.map(e=>e.id)")

                        initial = ["source-0", "source-2", "source-4", "source-1", "source-3"]
                        assert source_order() == initial
                        middle = page.locator("#source-2")
                        middle.evaluate("el=>el.scrollIntoView({block:'center'})")
                        middle.focus()
                        viewport_before = page.locator('#rules').evaluate("e=>e.scrollTop")
                        middle_y = middle.bounding_box()["y"]
                        page.keyboard.press("Space")
                        assert not middle.is_checked() and source_order() == initial
                        assert middle.evaluate("el=>document.activeElement===el"), "toggle lost keyboard focus"
                        assert abs(page.locator('#rules').evaluate("e=>e.scrollTop") - viewport_before) <= 1, "switch scrolled the page"
                        assert abs(middle.bounding_box()["y"] - middle_y) <= 1, "switch jumped in viewport"
                        page.locator("#source-1").check()
                        assert source_order() == initial, "enabling moved a draft row"
                        page.locator("#source-3").check()
                        assert source_order() == initial, "built-in draft row moved"
                        page.evaluate("previewSaveFails=true")
                        page.locator("#saveSources").click()
                        page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('保存失败')")
                        assert source_order() == initial, "failed save changed order"
                        assert not page.locator("#saveSources").is_disabled()
                        page.evaluate("previewSaveFails=false;previewSaveDelay=200")
                        page.locator("#saveSources").click()
                        assert source_order() == initial, "save request sorted before success"
                        page.wait_for_function("document.querySelector('#sourcesStatus').textContent.startsWith('已保存')")
                        saved_order = ["source-0", "source-1", "source-3", "source-4", "source-2"]
                        assert source_order() == saved_order
                        page.locator("#source-4").uncheck()
                        assert source_order() == saved_order
                        page.locator("#source-4").check()
                        assert source_order() == saved_order and page.locator("#saveSources").is_disabled()
                    page.close()
            module_update_regression(browser, errors, args.screenshots)
            update_failure_feedback_regression(browser, errors, args.screenshots)
            adaptive_regression(browser, args.screenshots, errors)
            assert not errors, errors
            print("Vue WebUI phone/fold/tablet viewport/theme combinations, offline loading, compact header, QUIC preferences, fold/rotate drafts and functional regressions passed")
        finally:
            browser.close()


if __name__ == "__main__":
    main()
