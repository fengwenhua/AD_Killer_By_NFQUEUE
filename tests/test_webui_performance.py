#!/usr/bin/env python3
"""Offline production-artifact stress probe. Timings are evidence, not CI FPS guarantees."""
import argparse
import json
from pathlib import Path
import tempfile
import zipfile
from playwright.sync_api import sync_playwright
from test_webui_layout import ROOT, mock_script


def measure(browser, html, sources=64, cpu=4, running=False):
    page = browser.new_page(viewport={'width': 393, 'height': 852})
    errors = []
    page.on('pageerror', lambda error: errors.append(str(error)))
    page.add_init_script(mock_script([{'url': f'https://example.org/source{i}', 'enabled': True} for i in range(sources)]) + r'''
      ksu.listPackages=()=>JSON.stringify(Array.from({length:800},(_,i)=>`org.example.app${i}`));
      ksu.getPackagesInfo=p=>JSON.stringify(JSON.parse(p).map(packageName=>({packageName,appLabel:packageName})));
    ''' + ("\npreviewEngineState='running';" if running else ''))
    page.goto(html.resolve().as_uri())
    page.wait_for_function("document.querySelectorAll('.app').length > 0 && document.querySelector('#appList').getAttribute('aria-busy')==='false'")
    page.evaluate("document.querySelector('#sourcesDetails').open=true")
    page.evaluate('document.fonts.ready')
    page.evaluate('new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)))')
    client = page.context.new_cdp_session(page)
    client.send('Performance.enable')
    client.send('Emulation.setCPUThrottlingRate', {'rate': cpu})
    before = {item['name']: item['value'] for item in client.send('Performance.getMetrics')['metrics']}
    sample = page.evaluate('''async () => {
      const frames=[],bounds=[],steps=[];
      let previous=performance.now(), stop=false;
      function frame(now) {
        if(stop)return;
        frames.push(now-previous);previous=now;
        const r=document.querySelector('nav').getBoundingClientRect();
        bounds.push([r.x,r.y,r.width,r.height]);requestAnimationFrame(frame);
      }
      requestAnimationFrame(frame);
      for(const id of ['apps','rules','settings','home','rules','apps','home']) {
        steps.push({id,at:performance.now()});
        document.querySelector(`nav [data-page=${id}]`).click();
        await new Promise(resolve=>setTimeout(resolve,400));
      }
      await new Promise(resolve=>setTimeout(resolve,300));stop=true;
      return {frames,bounds,steps,windowScroll:scrollY,
        pageSizes:[...document.querySelectorAll('.page')].map(e=>[e.clientHeight,document.querySelector('main').clientHeight]),
        apps:document.querySelectorAll('.app').length};
    }''')
    after = {item['name']: item['value'] for item in client.send('Performance.getMetrics')['metrics']}
    frames = sorted(value for value in sample['frames'] if value > 0)
    report = {'sources': sources, 'apps': 800, 'cpu_slowdown': cpu, 'engine_running': running, 'rendered_apps': sample['apps'],
              'frames': len(frames), 'p95_ms': round(frames[int(len(frames) * .95)], 2), 'max_ms': round(max(frames), 2),
              'layouts': after['LayoutCount'] - before['LayoutCount'],
              'style_ms': round((after['RecalcStyleDuration'] - before['RecalcStyleDuration']) * 1000, 2),
              'task_ms': round((after['TaskDuration'] - before['TaskDuration']) * 1000, 2),
              'nav_bounds_count': len(set(map(tuple, sample['bounds'])))}
    assert not errors, errors
    if running and page.locator('.guard-halo').count():
        report['running_indicator'] = page.locator('.guard-halo').evaluate("e=>getComputedStyle(e).animationName==='guard-breathe'")
    if page.locator('dialog').count():
        feedback = page.evaluate('''async () => {
          document.querySelector('nav [data-page=rules]').click();
          await new Promise(resolve=>setTimeout(resolve,350));
          previewContentDelay=5000;
          const button=document.querySelector('.source-item .preview-button');
          button.scrollIntoView({block:'center'});
          const start=performance.now();button.click();
          await new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve)));
          const dialog=document.querySelector('dialog');
          return {ms:performance.now()-start,open:dialog.open,
            loading:dialog.textContent.includes('正在读取来源开头')};
        }''')
        report.update(preview_feedback_ms=round(feedback['ms'], 2), preview_immediate_loading=feedback['open'] and feedback['loading'])
    assert not errors, errors
    return page, report, sample


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--html', type=Path, default=ROOT / 'module/webroot/index.html')
    parser.add_argument('--sources', type=int, default=64)
    parser.add_argument('--cpu', type=int, default=4)
    parser.add_argument('--verify', action='store_true')
    parser.add_argument('--running', action='store_true')
    parser.add_argument('--baseline-zip', type=Path)
    args = parser.parse_args()
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch()
        try:
            if args.baseline_zip:
                with tempfile.TemporaryDirectory(prefix='ad_killer_by_nfqueue_perf_') as directory:
                    baseline = Path(directory) / 'index.html'
                    with zipfile.ZipFile(args.baseline_zip) as archive:
                        baseline.write_bytes(archive.read('webroot/index.html'))
                    page, report, _ = measure(browser, baseline, args.sources, args.cpu, args.running)
                    print(json.dumps({'baseline_zip': str(args.baseline_zip), **report}), flush=True)
                    page.close()
            page, report, sample = measure(browser, args.html, args.sources, args.cpu, args.running)
            print(json.dumps(report), flush=True)
            if args.verify:
                assert report['rendered_apps'] < 40, report
                assert report['nav_bounds_count'] == 1, report
                assert report['preview_immediate_loading'], report
                if args.running:
                    assert report.get('running_indicator'), report
                assert sample['windowScroll'] == 0
                assert all(height == stage and height > 0 for height, stage in sample['pageSizes'])
                assert page.locator('nav').evaluate("e=>getComputedStyle(e).backdropFilter") == 'none'
                assert page.locator('.nav-indicator').evaluate("e=>getComputedStyle(e).transitionProperty") == 'transform'
            page.close()
        finally:
            browser.close()


if __name__ == '__main__':
    main()
