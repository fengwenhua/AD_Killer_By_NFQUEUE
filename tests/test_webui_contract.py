#!/usr/bin/env python3
"""Validate the actual offline Vue build (runtime behavior lives in frontend/tests)."""
import re
import subprocess
import sys
import tempfile
from html.parser import HTMLParser
from pathlib import Path


class Page(HTMLParser):
    def __init__(self):
        super().__init__()
        self.scripts = []
        self.external = []
        self.in_script = False

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == 'script':
            self.in_script = True
            self.scripts.append('')
            assert not attrs.get('src'), 'offline build must inline all JavaScript'
        if tag == 'link' and attrs.get('rel') in ('stylesheet', 'modulepreload'):
            self.external.append(attrs.get('href'))

    def handle_endtag(self, tag):
        if tag == 'script':
            self.in_script = False

    def handle_data(self, data):
        if self.in_script:
            self.scripts[-1] += data


def main():
    html = Path(sys.argv[1]).read_text(encoding='utf-8')
    page = Page()
    page.feed(html)
    assert not page.external, f'external offline assets: {page.external}'
    assert page.scripts and len(''.join(page.scripts)) > 20000, 'expected compiled Vue runtime/application'
    assert 'id="app"' in html and '<style' in html
    assert 'Bundled runtime: Vue (MIT)' in html and 'Yuxi (Evan) You' in html, 'retain Vue runtime license'
    assert 'Bundled runtime: GSAP (Standard License)' in html and 'Copyright 2008-2026, GreenSock' in html and 'https://gsap.com/standard-license' in html, 'retain GSAP notice and license link'
    assert not re.search(r'\son(?:click|change|input)=', html), 'no legacy inline event handlers'
    assert not re.search(r'https?://[^\s"<>]+\.(?:js|css)["\s<]', html), 'no CDN runtime'
    assert not re.search(r'\beval\(', ''.join(page.scripts)), 'no runtime evaluation of legacy HTML'
    with tempfile.TemporaryDirectory() as directory:
        for index, script in enumerate(page.scripts):
            path = Path(directory) / f'bundle-{index}.mjs'
            path.write_text(script, encoding='utf-8')
            subprocess.run(['node', '--check', str(path)], check=True)
    print('compiled Vue offline WebUI contract passed')


if __name__ == '__main__':
    main()
