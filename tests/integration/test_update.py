#!/usr/bin/env python3
"""Real CLI publishing tests; root only inside a disposable Linux container."""
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import threading
import time


DATA = Path("/data/adb/AD_Killer_By_NFQUEUE")
BINARY = sys.argv[1]
HAGEZI = "builtin:hagezi-pro-minus-fake"
FIXTURES = {
    "/adguard": b"||ads.example.com^\n@@||safe.example.com^\n||ads.example.com^\n",
    "/prefix-mask": b"||ads.example.com^\n||adservice.google.\n@@||allow.example.\n",
    "/hosts": b"0.0.0.0 tracker.example.org\n",
    "/empty": b"",
    "/html": b"<!doctype html><html><body>Error</body></html>",
    "/bad-gzip": b"\x1f\x8b\x08\x00corrupt",
}
SLOW_STARTED = threading.Event()
SLOW_RELEASE = threading.Event()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/slow":
            SLOW_STARTED.set()
            SLOW_RELEASE.wait(10)
            self.send_response(200)
            self.end_headers()
            self.wfile.write(FIXTURES["/hosts"])
            return
        self.send_response(200 if self.path in FIXTURES else 404)
        self.end_headers()
        self.wfile.write(FIXTURES.get(self.path, b"not found"))


def command(*args, success=True):
    result = subprocess.run([BINARY, *args], text=True, capture_output=True, timeout=20)
    assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
    return result.stdout


def main():
    assert sys.platform == "linux" and os.geteuid() == 0
    assert not DATA.exists(), "refusing existing module data"
    DATA.parent.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        command("--install")
        defaults = json.loads(command("--sources-list"))
        recipe_count=sum(line.startswith('RULE_SOURCE(') for line in (Path(__file__).resolve().parents[2]/'src/rule_recipe.inc').read_text().splitlines())
        assert len(defaults["sources"]) == recipe_count + 1
        ordinary = [s for s in defaults["sources"] if not s["locked"]]
        special = [s for s in defaults["sources"] if s["locked"]]
        assert len(ordinary) == recipe_count and all(s["enabled"] for s in ordinary)
        assert not any(s['url']=='https://adrules.top/adrules.txt' for s in ordinary)
        assert len(special) == 1 and special[0]["url"] == HAGEZI and special[0]["enabled"]
        assert len(special[0]["dependencies"]) == 2
        assert all("hagezi" not in s["url"].lower() for s in ordinary)
        config=DATA/'state.conf'
        config.write_text(''.join(line for line in config.read_text().splitlines(keepends=True) if not line.startswith('source_defaults_version=')))
        sources_file=DATA/'update_sources.txt'
        sources_file.write_text('# mine\nhttps://example.org/user\n# disabled: https://example.org/off\nhttps://adrules.top/adrules.txt\n# disabled: '+HAGEZI+'\n')
        command('--install')
        assert sources_file.read_text()=='# mine\nhttps://example.org/user\n# disabled: https://example.org/off\n'+HAGEZI+'\n'
        sources_file.write_text('# mine\nhttps://example.org/user\n# disabled: '+HAGEZI+'\n')
        command('--install')
        assert '# disabled: '+HAGEZI in sources_file.read_text(), 'later manual builtin opt-out must survive reinstall'
        # Exact old defaults upgrade, but edited lists and pause state survive.
        (DATA / "update_sources.txt").write_text(
            "# 每行一个规则源 URL,支持 Hosts / AdGuard / gzip(.gz)\n"
            "# 以下为默认源,可自行增删\n"
            "https://anti-ad.net/easylist.txt\n"
            "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n")
        before_config = (DATA / "state.conf").read_bytes()
        command("--install")
        assert len(json.loads(command("--sources-list"))["sources"]) == recipe_count + 1
        assert (DATA / "state.conf").read_bytes() == before_config
        (DATA / "update_sources.txt").write_text("# custom\nhttps://example.org/user\n")
        command("--install")
        assert (DATA / "update_sources.txt").read_text() == "# custom\nhttps://example.org/user\n"
        def save_sources(text, success=True):
            result = subprocess.run([BINARY, "--sources-save"], input=text, text=True,
                                    capture_output=True, timeout=20)
            assert (result.returncode == 0) == success, result.stderr
            return result

        original_sources = (DATA / "update_sources.txt").read_bytes()
        for invalid in ("file:///etc/passwd\n", "https://x.example/a\nhttps://x.example/a\n",
                        "# disabled: https://user:pass@example.org/a\n"):
            save_sources(invalid, success=False)
            assert (DATA / "update_sources.txt").read_bytes() == original_sources

        # The immutable recipe is always restored, including when an older UI
        # omits it on save; enabling/disabling still persists normally.
        for enabled in (True, False):
            save_sources(("" if enabled else "# disabled: ") + HAGEZI + "\n")
            configured = json.loads(command("--sources-list"))["sources"]
            assert len(configured) == 1 and configured[0]["locked"] and configured[0]["enabled"] is enabled
            save_sources("https://example.org/ordinary\n")
            configured = json.loads(command("--sources-list"))["sources"]
            assert configured[-1]["url"] == HAGEZI and configured[-1]["enabled"] is enabled
        protected_config = (DATA / "update_sources.txt").read_bytes()
        save_sources("builtin:hagezi-edited\n", success=False)
        assert (DATA / "update_sources.txt").read_bytes() == protected_config

        def wait_job(started):
            for _ in range(100):
                result = json.loads(command("--update-status", started["job_id"]).splitlines()[-1])
                if result["status"] != "running":
                    return result
                time.sleep(0.05)
            raise AssertionError("job did not finish")

        def sources(*paths):
            (DATA / "update_sources.txt").write_text("".join(
                f"http://127.0.0.1:{server.server_port}{path}\n" for path in paths))

        for paths, counts in ((("/adguard",), (1, 1)), (("/hosts",), (1, 0)),
                              (("/adguard", "/hosts"), (2, 1))):
            sources(*paths)
            command("--update")
            generation = (DATA / "rules.current").read_text().strip()
            base = DATA / "rulesets" / generation
            for name in ("hosts_rules.gz", "adguard_rules.gz"):
                assert gzip.decompress((base / name).read_bytes()).startswith(b"# AD_Killer_By_NFQUEUE")
            stats = dict(line.split("=", 1) for line in
                         (DATA / "available_rule_stats.conf").read_text().splitlines())
            assert (int(stats["black"]), int(stats["white"])) == counts, stats
            assert not (DATA / "daemon.pid").exists(), "updating started an engine"
            assert not (DATA / "rule_stats.conf").exists(), "sync overwrote active-rule counters without an engine ACK"
            summary = dict(line.split("=", 1) for line in
                           (DATA / "source_stats.conf").read_text().split("\n[source_0]")[0].splitlines())
            assert int(summary["unique_total"]) == sum(counts)
            assert int(summary["duplicate_lines"]) == (1 if "/adguard" in paths else 0)
            assert summary["generation"] == generation

        # Disabled source must survive restart and never be downloaded.
        prefix = f"http://127.0.0.1:{server.server_port}"
        save_sources(f"{prefix}/prefix-mask\n# disabled: {prefix}/404\n")
        configured = json.loads(command("--sources-list"))["sources"]
        assert len(configured) == 3 and not configured[1]["enabled"] and configured[2]["locked"]
        command("--update")
        report = (DATA / "source_stats.conf").read_text()
        assert "source_count=1\n" in report and "filtered_lines=2\n" in report
        assert "unique_total=1\n" in report

        # Testing good/bad/disabled/unsaved URLs must not touch published data,
        # source settings, counters, boot policy, or daemon state.
        protected = ["rules.current", "source_stats.conf", "available_rule_stats.conf",
                     "state.conf", "update_sources.txt", "apply.state"]
        snapshot = {p: (DATA / p).read_bytes() for p in protected}
        for path, status in (("/hosts", "success"), ("/html", "failed"), ("/404", "failed")):
            started = json.loads(command("--source-test", prefix + path).splitlines()[-1])
            assert wait_job(started)["status"] == status
            assert {p: (DATA / p).read_bytes() for p in protected} == snapshot
            log = (DATA / "update_jobs" / f"update-{started['job_id']}.log").read_text()
            assert "仅测试下载和解析" in log
            assert not (DATA / "daemon.pid").exists()
        command("--source-test", "file:///etc/passwd", success=False)

        started = json.loads(command("--source-test", prefix + "/slow").splitlines()[-1])
        assert SLOW_STARTED.wait(5)
        try:
            save_sources("https://example.org/concurrent\n", success=False)
            command("--install", success=False)
            command("--update", success=False)
            assert {p: (DATA / p).read_bytes() for p in protected} == snapshot
        finally:
            SLOW_RELEASE.set()
        assert wait_job(started)["status"] == "success"
        assert {p: (DATA / p).read_bytes() for p in protected} == snapshot

        save_sources("")
        command("--update", success=False)
        assert (DATA / "rules.current").read_bytes() == snapshot["rules.current"]

        manifest = (DATA / "rules.current").read_bytes()
        for path in ("/empty", "/html", "/bad-gzip", "/404"):
            sources(path)
            command("--update", success=False)
            assert (DATA / "rules.current").read_bytes() == manifest
            assert not list((DATA / "rulesets").glob(".staging-*"))

        sources("/404", "/adguard")
        started = json.loads(command("--update-start").splitlines()[-1])
        assert started["status"] == "started"
        for _ in range(100):
            result = json.loads(command("--update-status", started["job_id"]).splitlines()[-1])
            if result["status"] != "running":
                break
            time.sleep(0.05)
        assert result["status"] == "success" and result["exit_code"] == 0, result
        assert (DATA / "rules.current").read_bytes() != manifest
        assert "publish_status=published" in (DATA / "source_stats.conf").read_text()
        report = (DATA / "source_stats.conf").read_text()
        assert "accepted_sources=1\n" in report and "rejected_sources=1\n" in report
        assert "duplicate_lines=1\n" in report
        assert f"job_id={started['job_id']}\n" in report
        sections = report.split("\n[source_")[1:]
        assert len(sections) == 2
        for section in sections:
            fields = dict(line.split("=", 1) for line in section.splitlines() if "=" in line)
            assert int(fields["checked_at"]) > 0
        assert not (DATA / "daemon.pid").exists()
        print("real update AdGuard-only/Hosts-only/mixed, paused counters, bad-source "
              "preservation and asynchronous job tests passed")
    finally:
        SLOW_RELEASE.set()
        server.shutdown()
        server.server_close()
        worker.join()
        if DATA.exists():
            shutil.rmtree(DATA)


if __name__ == "__main__":
    main()
