#!/usr/bin/env python3
"""Real compiled stable/preview UIs; all Android operations are mocked."""
from playwright.sync_api import expect, sync_playwright
from test_webui_layout import ROOT, VERSION_FIXTURE, mock_script
import json


def main():
    errors = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            for installed, target in (('preview','stable'),('stable','stable'),('stable','preview'),('unknown','stable')):
                path = ROOT / ('dist/webui-channel-preview/index.html' if installed == 'preview' else 'module/webroot/index.html')
                version = VERSION_FIXTURE['version']
                info = {**VERSION_FIXTURE, 'releaseChannel': installed,
                        'versionCode': VERSION_FIXTURE['versionCode'] - (installed == 'preview')}
                setup = mock_script() + '\npreviewVersion=' + json.dumps(info) + ';\n' + r'''
                  const currentVersion=previewVersion.version;
                  const targetChannel=__TARGET__;
                  const original=ksu.exec.bind(ksu);
                  let ready='', reboot=false;
                  ksu.exec=(command,options,callback)=>{
                    let value;
                    if(command.includes(' --module-update-check ')) {
                      const base='https://github.com/fengwenhua/AD_Killer_By_NFQUEUE/releases';
                      const name=`AD_Killer_By_NFQUEUE-v${currentVersion}.zip`, url=`${base}/download/v${currentVersion}/${name}`;
                      value={tag_name:'v'+currentVersion,draft:false,prerelease:targetChannel==='preview',html_url:`${base}/tag/v${currentVersion}`,
                        assets:[{name,size:500,browser_download_url:url},{name:name+'.sha256',size:96,browser_download_url:url+'.sha256'}]};
                    } else if(command.includes(' --module-update-status')) value={version:ready,channel:targetChannel,rebootRequired:reboot};
                    else if(command.includes(' --module-update-download ')) {ready=currentVersion;value={status:'started',job_id:'987-654'};}
                    else if(command.includes(' --module-update-install ')) {reboot=true;value={status:'started',job_id:'987-655'};}
                    if(value) {previewCommands.push(command);setTimeout(()=>window[callback](0,JSON.stringify(value),''),0);}
                    else original(command,options,callback);
                  };
                '''.replace('__TARGET__', json.dumps(target))
                page = browser.new_page(viewport={'width':393,'height':852},reduced_motion='reduce')
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.add_init_script(setup)
                page.goto(path.as_uri())
                page.locator('nav [data-page="settings"]').click()
                page.locator('[data-settings-entry="about"]').click()
                expect(page.locator('#aboutVersion')).to_contain_text({'preview':'预览版','stable':'正式版','unknown':'渠道未知'}[installed])
                expect(page.locator('#versionMismatch')).not_to_be_visible()
                page.locator('#settingsBack').click()
                page.locator('[data-settings-entry="updates"]').click()
                if target == 'preview':
                    page.locator('#updatePreview').check()
                page.locator('#getUpdate').click()
                expect(page.locator('#getUpdate')).to_be_enabled()
                if installed == 'preview' and target == 'stable':
                    expect(page.locator('#moduleUpdateStatus')).to_contain_text('预览版升级正式版')
                    page.locator('#downloadModuleUpdate').click()
                    expect(page.locator('#installModuleUpdate')).to_be_enabled()
                    assert not page.evaluate("previewCommands.some(c=>c.includes(' --module-update-install '))")
                    page.locator('#installModuleUpdate').click()
                    page.locator('#confirmModuleInstall').click()
                    expect(page.locator('#moduleUpdateStatus')).to_contain_text('安装成功')
                    expect(page.locator('.update-status b')).to_have_text('等待重启')
                    assert page.evaluate("previewCommands.some(c=>c.endsWith(' --module-update-download '+previewVersion.version))")
                    assert page.evaluate("previewCommands.some(c=>c.endsWith(' --module-update-install '+previewVersion.version))")
                else:
                    expect(page.locator('#downloadModuleUpdate')).not_to_be_visible()
                    expect(page.locator('#installModuleUpdate')).not_to_be_visible()
                    expect(page.locator('#moduleUpdateStatus')).to_contain_text('旧包未记录发布渠道' if installed == 'unknown' else '没有可升级版本')
                assert page.evaluate('document.documentElement.scrollWidth <= innerWidth')
                page.close()
                print(f'{installed} -> {target}, same v{version}: browser regression passed', flush=True)
            assert not errors, errors
        finally:
            browser.close()


if __name__ == '__main__':
    main()
