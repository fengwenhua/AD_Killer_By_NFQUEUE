#!/usr/bin/env python3
"""Exercise the real libcurl ABI and exec curl against a loopback HTTP/TLS server."""
import ctypes.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


ROOT = Path(__file__).resolve().parents[2]
LIMIT = 64 * 1024 * 1024


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        try:
            if self.path.startswith('/cancel-'):
                self.send_response(200)
                self.send_header('Content-Length', '1000')
                self.end_headers()
                self.wfile.flush()
                self.server.transfer_started.set()
                self.server.release_transfer.wait(10)
            elif self.path in ("/redirect", "/loop", "/file"):
                self.send_response(302)
                self.send_header("Location", {"/redirect": "/ok", "/loop": "/loop",
                                             "/file": "file:///etc/passwd"}[self.path])
                self.end_headers()
            elif self.path == "/known-large":
                self.send_response(200)
                self.send_header("Content-Length", str(LIMIT + 1))
                self.end_headers()
            elif self.path == "/stream-large":
                self.send_response(200)
                self.end_headers()
                for _ in range(1025):
                    self.wfile.write(b"x" * 65536)
            elif self.path == "/short":
                self.send_response(200)
                self.send_header("Content-Length", "1000")
                self.end_headers()
                self.wfile.write(b"short")
            else:
                self.send_response(200 if self.path == "/ok" else 404)
                self.end_headers()
                self.wfile.write(b"ads.example.com\n")
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    curl = shutil.which("curl")
    library = ctypes.util.find_library("curl")
    assert curl and library and shutil.which("openssl"), "requires curl, libcurl and openssl"
    with tempfile.TemporaryDirectory(prefix="ad_killer_by_nfqueue_download_") as directory:
        base = Path(directory)
        outputs = []
        for mode in ("libcurl", "exec"):
            binary = base / mode
            args = [os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-Wall", "-Wextra",
                    "-Wpedantic", "-Werror", "-pthread", "-I" + str(ROOT / "src"),
                    '-DADK_NFQ_TEST_CURL_LIBRARY="' + (library if mode == "libcurl" else
                                                      "/nonexistent/ad_killer_by_nfqueue_libcurl.so") + '"',
                    '-DADK_NFQ_TEST_CURL_PROGRAM="' + (curl if mode == "exec" else
                                                      "/nonexistent/ad_killer_by_nfqueue_curl") + '"',
                    str(ROOT / "tests/integration/download_probe.cpp")]
            args += [str(ROOT / "src" / name) for name in
                     ("net.cpp", "net_policy.cpp", "logging.cpp", "bounded_log.cpp")]
            args += ["-lz", "-ldl", "-o", str(binary)]
            subprocess.run(args, check=True)
            outputs.append(binary)

        cert = base / "cert.pem"
        key = base / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1",
                        "-subj", "/CN=localhost", "-addext", "subjectAltName=IP:127.0.0.1"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        servers = []
        threads = []
        try:
            for tls in (False, True):
                server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
                server.transfer_started = threading.Event()
                server.release_transfer = threading.Event()
                if tls:
                    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                    context.load_cert_chain(cert, key)
                    server.socket = context.wrap_socket(server.socket, server_side=True)
                thread = threading.Thread(target=server.serve_forever, daemon=True)
                thread.start()
                servers.append(server)
                threads.append(thread)
            # If curl reads this config, the self-signed certificate test would
            # incorrectly succeed. --disable must be the first curl option.
            (base / ".curlrc").write_text("insecure\n")
            env = dict(os.environ, CURL_HOME=str(base), NO_PROXY="127.0.0.1,localhost")
            for binary in outputs:
                cases = [("http", servers[0], path, path in ("/ok", "/redirect")) for path in
                         ("/ok", "/redirect", "/404", "/loop", "/file",
                          "/known-large", "/stream-large", "/short")]
                cases.append(("https", servers[1], "/ok", False))
                for scheme, server, path, success in cases:
                    target = base / "rules.txt"
                    target.write_bytes(b"old rules\n")
                    url = f"{scheme}://127.0.0.1:{server.server_port}{path}"
                    result = subprocess.run([str(binary), url, str(target)], env=env,
                                            capture_output=True, text=True, timeout=15)
                    assert (result.returncode == 0) == success, \
                        (binary.name, path, result.stdout, result.stderr)
                    expected = b"ads.example.com\n" if success else b"old rules\n"
                    assert target.read_bytes() == expected, (binary.name, path, "old rules replaced")
                    assert not list(base.glob("rules.txt.tmp.*")), "download temp leaked"
                # An idle response has no write callbacks: cancellation must
                # use libcurl progress or the owned exec child's polling loop.
                for action in ('file', 'prefix'):
                    server = servers[0]
                    server.transfer_started.clear()
                    server.release_transfer.clear()
                    marker = base / 'closing'
                    marker.unlink(missing_ok=True)
                    target = base / 'rules.txt'
                    target.write_bytes(b'old rules\n')
                    child = subprocess.Popen([str(binary), f'http://127.0.0.1:{server.server_port}/cancel-{action}',
                                              str(target), str(marker), action], env=env,
                                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    try:
                        assert server.transfer_started.wait(5), (binary.name, action, 'request not started')
                        started = time.monotonic()
                        marker.write_text('uninstall requested\n')
                        stdout, stderr = child.communicate(timeout=5)
                        assert child.returncode == 1 and '卸载' in stderr, (binary.name, action, stdout, stderr)
                        assert time.monotonic() - started < 5
                        assert target.read_bytes() == b'old rules\n'
                        assert not list(base.glob('rules.txt.tmp.*'))
                    finally:
                        server.release_transfer.set()
                        marker.unlink(missing_ok=True)
                        if child.poll() is None:
                            child.kill()
                            child.wait()
            print("real libcurl/exec curl HTTP redirects, errors, TLS rejection, "
                  "64 MiB limits, idle cancellation and atomic old-file preservation tests passed")
        finally:
            for server in servers:
                server.release_transfer.set()
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()


if __name__ == "__main__":
    main()
