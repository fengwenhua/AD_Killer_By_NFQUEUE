#!/usr/bin/env python3
"""Real paired HTTP downloads + CLI jobs/publishing, disposable Linux root only."""
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
DATA = Path("/data/adb/AD_Killer_By_NFQUEUE")
ID = "builtin:hagezi-pro-minus-fake"
PRO = b"# Pro\n*.keep.example.com\n*.fake.example.com\n*.other.example.com\nkeep.example.com\n"
FAKE = b"# Fake\n*.fake.example.com\nnot-in-pro.example.org\n"
LONG = ('中😀rules\n' * 2000).encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.requests.append(self.path)
        payload = {"/pro": PRO, "/fake": self.server.fake,
                   "/ordinary": b"fake.example.com\n", "/long": LONG}.get(self.path)
        self.send_response(200 if payload is not None else 404)
        self.end_headers()
        self.wfile.write(b"missing" if payload is None else payload)


def main():
    assert os.geteuid() == 0 and not DATA.exists(), "disposable root environment required"
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.fake, server.requests = FAKE, []
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    prefix = f"http://127.0.0.1:{server.server_port}"
    with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_hagezi_live_") as directory:
        binary = str(Path(directory) / "AD_Killer_By_NFQUEUE")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O1", "-pthread",
                        "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                        '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"',
                        '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"',
                        f'-DADK_NFQ_TEST_HAGEZI_PRO_URL="{prefix}/pro"',
                        f'-DADK_NFQ_TEST_HAGEZI_FAKE_URL="{prefix}/fake"',
                        "-I" + str(ROOT / "src"), *map(str, sorted((ROOT / "src").glob("*.cpp"))),
                        "-ldl", "-lz", "-o", binary], check=True)

        def command(*args, success=True, text=None):
            result = subprocess.run([binary, *args], input=text, capture_output=True, text=True, timeout=20)
            assert (result.returncode == 0) == success, (args, result.stdout, result.stderr)
            return result.stdout

        def configure(enabled, ordinary=True):
            rows = (prefix + "/ordinary\n" if ordinary else "") + ("" if enabled else "# disabled: ") + ID + "\n"
            command("--sources-save", text=rows)

        def wait_job(started):
            for _ in range(200):
                state = json.loads(command("--update-status", started["job_id"]))
                if state["status"] != "running":
                    return state
                time.sleep(0.025)
            raise AssertionError("source job timeout")

        try:
            DATA.parent.mkdir(parents=True, exist_ok=True)
            for invalid in ('99', '5001', '1.5', '500;echo', '99999999999999999999'):
                command('--source-preview', prefix + '/long', invalid, success=False)
            assert not server.requests and not DATA.exists(), 'invalid preview count downloaded or wrote data'
            preview = json.loads(command("--source-preview", prefix + "/ordinary"))
            assert preview["previews"][0]["text"] == "fake.example.com\n"
            assert not DATA.exists(), "preview created runtime data before install"
            for count in (None, 100, 1000, 5000):
                args = () if count is None else (str(count),)
                preview = json.loads(command('--source-preview', prefix + '/long', *args))['previews'][0]
                expected = count or 500
                assert preview['text'] == LONG.decode()[:expected] and preview['characters'] == expected
                assert preview['truncated'] and not DATA.exists()
            server.requests.clear()
            command("--install")
            configure(False)
            command("--update")
            assert server.requests == ["/ordinary"], "disabled pair was downloaded"
            protected = ["state.conf", "update_sources.txt", "rules.current", "available_rule_stats.conf", "source_stats.conf", "apply.state"]
            before = {name: (DATA / name).read_bytes() for name in protected}
            preview = json.loads(command("--source-preview", ID))
            assert preview["status"] == "ok" and len(preview["previews"]) == 2
            assert preview["previews"][0]["text"] == PRO.decode()[:500]
            assert preview["previews"][1]["text"] == FAKE.decode()[:500]
            assert {name: (DATA / name).read_bytes() for name in protected} == before
            server.fake = None
            preview = json.loads(command("--source-preview", ID, success=False))
            assert preview["previews"][0]["text"] and preview["previews"][1]["error"]
            assert {name: (DATA / name).read_bytes() for name in protected} == before
            server.fake = FAKE
            test = json.loads(command("--source-test", ID))
            assert wait_job(test)["status"] == "success"
            assert {name: (DATA / name).read_bytes() for name in protected} == before
            log = (DATA / "update_jobs" / f"update-{test['job_id']}.log").read_text()
            assert "差集剔除 1 / 保留 2" in log and "未替换规则" in log
            configure(True)
            command("--update")
            generation = (DATA / "rules.current").read_text().strip()
            text = gzip.decompress((DATA / "rulesets" / generation / "hosts_rules.gz").read_bytes()).decode()
            assert set(line for line in text.splitlines() if not line.startswith("#")) == {
                "keep.example.com", "other.example.com", "fake.example.com"}, "Fake changed other sources"
            report = (DATA / "source_stats.conf").read_text()
            assert "difference_removed=1\n" in report and "difference_base_unique=3\n" in report
            assert "unique_black=3\n" in report and "unique_white=0\n" in report

            configure(False)
            server.requests.clear()
            command("--update")
            assert server.requests == ["/ordinary"]
            configure(True, ordinary=False)
            manifest = (DATA / "rules.current").read_bytes()
            for bad_fake in (None, b"", b"<!doctype html><html>error</html>", b"\x1f\x8bcorrupt"):
                server.fake = bad_fake
                test = json.loads(command("--source-test", ID))
                assert wait_job(test)["status"] == "failed"
                assert (DATA / "rules.current").read_bytes() == manifest
                command("--update", success=False)
                assert (DATA / "rules.current").read_bytes() == manifest
                assert "status=content_rejected" in (DATA / "source_stats.conf").read_text()
                assert not (DATA / "hagezi-pro.part").exists() and not (DATA / "hagezi-fake.part").exists()
            configure(True)
            command("--update")  # Other source can publish; incomplete Pro must not leak.
            generation = (DATA / "rules.current").read_text().strip()
            text = gzip.decompress((DATA / "rulesets" / generation / "hosts_rules.gz").read_bytes()).decode()
            assert "keep.example.com" not in text and "fake.example.com" in text
            assert "rejected_sources=1" in (DATA / "source_stats.conf").read_text()
            assert not (DATA / "daemon.pid").exists()
            print("HaGeZi real HTTP paired jobs, disabled testing, enable/disable, scoped difference, "
                  "404/empty/HTML/gzip failures, atomic retention and no-engine-side-effect tests passed")
        finally:
            server.shutdown()
            server.server_close()
            if DATA.exists():
                shutil.rmtree(DATA)


if __name__ == "__main__":
    main()
