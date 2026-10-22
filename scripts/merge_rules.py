#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Reference merger: shared ordinary-source C++ recipe, no HaGeZi pair.

--fresh bypasses cache; --snapshot-dir freezes inputs; --from-snapshot replays
offline. Android uses the native merger, never this Python runtime.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import gzip
import hashlib
import io
import json
from pathlib import Path
import re
import ssl
import urllib.request

HERE = Path(__file__).resolve().parent
CACHE = HERE.parent / "data/cache"
OUT = HERE.parent / "data/out_rules"
MAX_BYTES = 64 * 1024 * 1024
MAX_TOTAL_BYTES = 256 * 1024 * 1024
MAX_LINES = 5_000_000
DOM_RE = re.compile(r"[a-z0-9](?:[a-z0-9-]*[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]*[a-z0-9])?)+")


def read_recipe():
    pattern = re.compile(r'RULE_SOURCE\(("[^"\n]+"), ("[^"\n]+"), (ADGUARD|HOSTS_AS_ADGUARD)\)')
    result = []
    for line in (HERE.parent / "src/rule_recipe.inc").read_text().splitlines():
        if not line or line.startswith("//"):
            continue
        match = pattern.fullmatch(line)
        if not match:
            raise ValueError("invalid shared recipe: " + line)
        name, url, kind = match.groups()
        result.append((json.loads(name), json.loads(url), "",
                       "hosts" if kind == "HOSTS_AS_ADGUARD" else "adguard"))
    if not 1 <= len(result) <= 64 or len({url for _, url, _, _ in result}) != len(result) or any("hagezi" in url.lower() for _, url, _, _ in result):
        raise ValueError("expected 1..64 unique ordinary sources without HaGeZi")
    return result


ADGUARD_SOURCES = read_recipe()


def valid_domain(domain):
    return (len(domain) <= 253 and DOM_RE.fullmatch(domain) is not None
            and all(len(label) <= 63 for label in domain.split(".")))


def parse_adguard_line(line):
    s = line.strip(" \t\r\n")
    if not s or len(s.encode("utf-8")) > 1024 or s[0] in "!#[":
        return None
    if any(marker in s for marker in ("##", "#@#", "#$#", "#%#", "#?#")):
        return None
    is_white = s.startswith("@@")
    if is_white:
        s = s[2:].lstrip(" \t")
    if not s or "badfilter" in s:
        return None
    suffix = s.startswith(("*.", "||"))
    if suffix:
        s = s[2:]
    elif s.startswith("|"):
        return None
    if s.endswith("^"):
        s = s[:-1]
    if any(c in s for c in "^$*/| \t"):
        return None
    if not s.isascii():
        return None
    domain = s.lower()
    if not valid_domain(domain):
        return None
    kind = ("white_" if is_white else "black_") + ("suffix" if suffix else "exact")
    canonical = ("@@" if is_white else "") + ("||" if suffix else "") + domain + ("^" if suffix else "")
    return kind, domain, canonical


def parse_adguard_text(text):
    black, white, canonical = set(), set(), set()
    for line in text.split("\n"):
        parsed = parse_adguard_line(line)
        if parsed:
            kind, domain, normalized = parsed
            key = ("suffix" if kind.endswith("suffix") else "exact", domain)
            (white if kind.startswith("white") else black).add(key)
            canonical.add(normalized)
    return black, white, canonical


def parse_hosts_text(text):
    domains = set()
    for line in text.split("\n"):
        s = line.strip(" \t\r\n")
        if not s or len(s.encode("utf-8")) > 1024 or s[0] in "!#;":
            continue
        if s.startswith("*."):
            s = s[2:]
        elif " " in s or "\t" in s:
            parts = s.split()
            if len(parts) < 2 or parts[0] not in ("0.0.0.0", "127.0.0.1"):
                continue
            s = parts[1]
        if s.endswith("."):
            s = s[:-1]
        if not s.isascii():
            continue
        s = s.lower()
        if valid_domain(s):
            domains.add(s)
    return domains


def decode_source(data):
    if not data or len(data) > MAX_BYTES:
        raise ValueError("empty_or_oversize")
    if data.startswith(b"\x1f\x8b"):
        with gzip.GzipFile(fileobj=io.BytesIO(data)) as stream:
            data = stream.read(MAX_TOTAL_BYTES + 1)
    if len(data) > MAX_TOTAL_BYTES:
        raise ValueError("decompressed_size_limit")
    return data.decode("utf-8", "replace"), len(data)


def source_rules(data, source_format):
    text, size = decode_source(data)
    prefix = text[:8192].lower()
    start = prefix.lstrip(" \t\r\n\ufeff")
    if start.startswith(("<!doctype", "<html")) or any(t in prefix for t in ("<head", "<body", "</html>")):
        raise ValueError("html_content")
    lines = text.count("\n") + (not text.endswith("\n"))
    if lines > MAX_LINES:
        raise ValueError("line_count_limit")
    rules, generated_bytes = set(), 0
    for line in text.split("\n"):
        if source_format == "hosts-as-adguard":
            normalized = parse_hosts_text(line)
        else:
            parsed = parse_adguard_line(line)
            normalized = {parsed[2]} if parsed else set()
        rules.update(normalized)
        generated_bytes += sum(len(rule) + 1 for rule in normalized)
    if not rules:
        raise ValueError("no_valid_rules")
    return rules, size, lines, generated_bytes


def fetch_bytes(url):
    # Certificate errors must never disable TLS verification.
    context = ssl.create_default_context()
    error = None
    for _ in range(2):
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "AD_Killer_By_NFQUEUE/recipe-v2"})
            with urllib.request.urlopen(request, timeout=60, context=context) as response:
                if not response.url.lower().startswith(("https://", "http://")):
                    raise ValueError("non-HTTP redirect")
                data = response.read(MAX_BYTES + 1)
            if not data or len(data) > MAX_BYTES:
                raise ValueError("empty_or_oversize")
            return data
        except Exception as exc:
            error = exc
    raise RuntimeError(str(error))


def cached_fetch(url, fresh=False):
    CACHE.mkdir(parents=True, exist_ok=True)
    path = CACHE / (hashlib.sha256(url.encode()).hexdigest() + ".txt")
    if not fresh and path.exists() and 0 < path.stat().st_size <= MAX_BYTES:
        return path.read_bytes()
    data = fetch_bytes(url)
    path.write_bytes(data)
    return data


def run_reference(inputs, out):
    merged, reports = set(), []
    used_bytes, used_lines, used_generated_bytes = 0, 0, 0
    for name, url, source_format, data, error in inputs:
        report = {"name": name, "url": url, "format": source_format}
        try:
            if error:
                raise RuntimeError(error)
            rules, size, lines, generated_bytes = source_rules(data, source_format)
            if (size > MAX_TOTAL_BYTES - used_bytes or lines > MAX_LINES - used_lines
                    or generated_bytes > MAX_TOTAL_BYTES - used_generated_bytes):
                raise ValueError("aggregate_resource_limit")
            merged.update(rules)
            used_bytes += size
            used_lines += lines
            used_generated_bytes += generated_bytes
            report.update(status="accepted", valid_unique=len(rules), bytes=size)
            print(f"  ✓ {name}: {len(rules):,} 条（单源去重后）", flush=True)
        except Exception as exc:
            report.update(status="rejected", reason=str(exc))
            print(f"  ✗ {name}: {exc}", flush=True)
        reports.append(report)
    if not merged:
        raise RuntimeError("所有源均不可用，不生成规则包")
    adguard = "# AD_Killer_By_NFQUEUE generated AdGuard rules\n" + "\n".join(sorted(merged)) + "\n"
    hosts = "# AD_Killer_By_NFQUEUE generated Hosts rules\n"
    black, white, _ = parse_adguard_text(adguard)
    out.mkdir(parents=True, exist_ok=True)
    for stem, text in (("hosts_rules", hosts), ("adguard_rules", adguard)):
        (out / (stem + ".txt")).write_text(text, encoding="utf-8")
        (out / (stem + ".gz")).write_bytes(gzip.compress(text.encode(), mtime=0))
    (out / "rule_stats.conf").write_text(f"mode=2\nblack={len(black)}\nwhite={len(white)}\nupdated_at=0\n")
    summary = dict(black=len(black), white=len(white), total=len(black) + len(white),
                   accepted_sources=sum(r["status"] == "accepted" for r in reports),
                   source_count=len(reports), sources=reports)
    (out / "merge_report.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    (out / "merge_report.txt").write_text(f"{len(reports)}-source recipe, no HaGeZi\n" + json.dumps(summary, ensure_ascii=False, indent=2) + "\n")
    print(json.dumps({k: v for k, v in summary.items() if k != "sources"}), flush=True)
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--fresh", action="store_true")
    parser.add_argument("--snapshot-dir", type=Path)
    parser.add_argument("--from-snapshot", type=Path)
    parser.add_argument("--out-dir", type=Path, default=OUT)
    args = parser.parse_args()
    if args.snapshot_dir and args.from_snapshot:
        parser.error("snapshot capture and replay are mutually exclusive")
    sources = [(name, url, "hosts-as-adguard" if fmt == "hosts" else "adguard")
               for name, url, _, fmt in ADGUARD_SOURCES]
    if args.from_snapshot:
        manifest = (args.from_snapshot / "sources.tsv").read_text().splitlines()
        if len(manifest) != len(sources):
            raise ValueError("snapshot source count mismatch")
        inputs = []
        for (name, url, fmt), line in zip(sources, manifest):
            actual_fmt, file, actual_url = line.split("\t")
            if (fmt, url) != (actual_fmt, actual_url) or not re.fullmatch(r"[0-9]{2}\.txt|-", file):
                raise ValueError("snapshot recipe mismatch")
            data = (args.from_snapshot / file).read_bytes() if file != "-" else b""
            inputs.append((name, url, fmt, data, "snapshot_download_failed" if file == "-" else ""))
    else:
        if args.snapshot_dir:
            args.snapshot_dir.mkdir(parents=True, exist_ok=False)
        def download(source):
            name, url, fmt = source
            print("拉取：" + name, flush=True)
            try:
                return name, url, fmt, cached_fetch(url, args.fresh), ""
            except Exception as exc:
                return name, url, fmt, b"", str(exc)
        with ThreadPoolExecutor(max_workers=3) as pool:
            inputs = list(pool.map(download, sources))
        if args.snapshot_dir:
            manifest, receipts = [], []
            for i, (name, url, fmt, data, error) in enumerate(inputs):
                file = "-" if error else f"{i:02d}.txt"
                if not error:
                    (args.snapshot_dir / file).write_bytes(data)
                manifest.append(f"{fmt}\t{file}\t{url}\n")
                receipts.append(dict(name=name, url=url, format=fmt, file=file, error=error,
                                     sha256=hashlib.sha256(data).hexdigest() if data else ""))
            (args.snapshot_dir / "sources.tsv").write_text("".join(manifest))
            (args.snapshot_dir / "receipts.json").write_text(json.dumps(receipts, ensure_ascii=False, indent=2) + "\n")
    run_reference(inputs, args.out_dir)


if __name__ == "__main__":
    main()
