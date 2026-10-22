#!/usr/bin/env python3
"""Visible overview: routing loss and stale image never remain green."""
from playwright.sync_api import expect, sync_playwright
from test_webui_layout import ROOT, mock_script


def main():
    errors = []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            page = browser.new_page(viewport={'width': 393, 'height': 852}, reduced_motion='reduce')
            page.on('pageerror', lambda error: errors.append(str(error)))
            page.add_init_script(mock_script() + "\npreviewEngineState='running';previewStartFails=false;")
            page.goto((ROOT/'module/webroot/index.html').as_uri())
            expect(page.locator('#stateTitle')).to_have_text('拦截已开启')
            page.evaluate('previewRoutingActive=false')
            expect(page.locator('#stateBadge')).to_have_text('路由待恢复', timeout=8000)
            expect(page.locator('#startBtn')).to_be_enabled()
            expect(page.locator('#stateDesc')).to_contain_text('启动守护')
            page.evaluate('previewRoutingActive=true')
            expect(page.locator('#stateTitle')).to_have_text('拦截已开启', timeout=8000)
            page.evaluate('previewDaemonImageCurrent=false')
            expect(page.locator('#stateBadge')).to_have_text('升级待生效', timeout=8000)
            expect(page.locator('#startBtn')).to_be_enabled()
            page.evaluate('previewDaemonImageCurrent=true')
            expect(page.locator('#stateTitle')).to_have_text('拦截已开启', timeout=8000)
            assert not page.evaluate("previewCommands.some(c=>c.endsWith(' --start') || c.endsWith(' --stop'))"), 'polling must not repair/restart by itself'
            assert page.evaluate('document.documentElement.scrollWidth<=innerWidth')
            assert not errors, errors
            print('Route loss, full revalidation, stale-image warning and live recovery browser checks passed')
        finally:
            browser.close()


if __name__ == '__main__':
    main()
