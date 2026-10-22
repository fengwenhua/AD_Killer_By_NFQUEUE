#!/usr/bin/env python3
"""Mobile counters and real localStorage reload persistence, mocked root bridge."""
from playwright.sync_api import expect, sync_playwright
from test_webui_layout import ROOT, mock_script


def main():
    errors=[]
    with sync_playwright() as playwright:
        browser=playwright.chromium.launch()
        try:
            page=browser.new_page(viewport={'width':393,'height':852},reduced_motion='reduce')
            page.on('pageerror',lambda error:errors.append(str(error)))
            page.add_init_script(mock_script()+"\npreviewSettings.block_quic=true;previewSettings.dev_mode=true;previewEngineState='running';")
            page.goto((ROOT/'module/webroot/index.html').as_uri())
            expect(page.locator('#quic')).to_be_checked()
            expect(page.locator('#devLog')).to_be_checked()
            expect(page.locator('#blockedTotal')).to_have_text('154')
            page.evaluate('previewMetrics.block_exact_hits=35')
            expect(page.locator('#blockedTotal')).to_have_text('155',timeout=7000)
            page.locator('nav [data-page="settings"]').click()
            page.locator('[data-settings-entry="releases"]').click()
            expect(page.locator('.release-list')).to_contain_text('来自 GitHub Release')
            assert page.evaluate("previewCommands.filter(c=>c.includes(' --module-release-notes')).length")==1
            page.locator('#settingsBack').click()
            page.locator('[data-settings-entry="updates"]').click()
            page.locator('#getUpdate').click()
            expect(page.locator('#downloadModuleUpdate')).to_be_visible()
            page.locator('#settingsBack').click()
            expect(page.locator('[data-settings-entry="updates"]')).to_contain_text('有更新')
            page.reload()
            page.locator('nav [data-page="settings"]').click()
            expect(page.locator('[data-settings-entry="updates"]')).to_contain_text('有更新')
            assert not page.evaluate("previewCommands.some(c=>c.includes(' --module-update-check '))"),'reopening must restore badge without a network check'
            page.locator('[data-settings-entry="releases"]').click()
            expect(page.locator('.release-list')).to_contain_text('来自 GitHub Release')
            assert not page.evaluate("previewCommands.some(c=>c.includes(' --module-release-notes'))"),'cached notes must not reload just because app reopened'
            expect(page.locator('#releaseNotes')).to_contain_text('已缓存')
            page.locator('#reloadReleaseNotes').click()
            expect(page.locator('#reloadReleaseNotes')).to_be_enabled()
            assert page.evaluate("previewCommands.filter(c=>c.includes(' --module-release-notes')).length")==1
            page.locator('#settingsBack').click()
            page.locator('[data-settings-entry="updates"]').click()
            expect(page.locator('#settings-updates')).to_contain_text('缓存结果')
            page.locator('#downloadModuleUpdate').click()
            expect(page.locator('#installModuleUpdate')).to_be_enabled()
            checks=page.evaluate("previewCommands.filter(c=>c.includes(' --module-update-check '))")
            assert len(checks)==1,'cached update must be checked again before download'
            assert not page.evaluate("previewCommands.some(c=>c.includes(' --module-update-install '))")
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
            assert not errors, errors
            print('Mobile default switches, live block counts, notes/update badge reload persistence and download recheck passed')
        finally:
            browser.close()


if __name__=='__main__':
    main()
