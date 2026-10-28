"""Reproducible synthetic seeds; no device traffic or downloaded lists."""
import gzip
import io
from pathlib import Path
import struct
import zipfile

TARGETS = {
    'packet': ['packet.cpp'],
    'rules': ['rules.cpp', 'rule_stream.cpp', 'rule_bundle.cpp', 'logging.cpp', 'bounded_log.cpp'],
    'nfnetlink': ['nfnetlink_parser.cpp'],
    'release_json': ['release_catalog.cpp'],
    'reassembly': ['tcp_reassembly.cpp', 'packet.cpp'],
    'dns_reassembly': ['dns_tcp_reassembly.cpp', 'tcp_reassembly.cpp', 'packet.cpp'],
    'rule_stream': ['rule_stream.cpp'],
    'module_zip': [],
}


def action(op=0, flow=0, flags=0, clock=1, sequence=100, payload=b''):
    return struct.pack('<BBBBIH', op, flow, flags, clock, sequence & 0xffffffff, len(payload)) + payload


def module_zip(compressed=False, duplicate=False, comment=b''):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED if compressed else zipfile.ZIP_STORED) as archive:
        info = zipfile.ZipInfo('module.prop', (1980, 1, 1, 0, 0, 0))
        info.compress_type = archive.compression
        archive.writestr(info, 'id=AD_Killer_By_NFQUEUE\nversion=v1.2.2\nversionCode=12201\nreleaseChannel=stable\n')
        if duplicate:
            # Duplicate module identity must be refused, not selected by order.
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter('ignore', UserWarning)
                archive.writestr(info, 'id=other\n')
        archive.comment = comment
    return stream.getvalue()


def seed_inputs(root):
    query = b'\x12\x34\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x03ads\x07example\x00\x00\x01\x00\x01'
    frame = struct.pack('!H', len(query)) + query
    http = b'GET / HTTP/1.1\r\nHost: ads.example\r\n\r\n'
    host = b'ads.example'
    sni = struct.pack('!HBH', len(host) + 3, 0, len(host)) + host
    extensions = struct.pack('!HH', 0, len(sni)) + sni
    body = b'\x03\x03' + b'B' * 32 + b'\x00\x00\x02\x13\x01\x01\x00' + struct.pack('!H', len(extensions)) + extensions
    handshake = b'\x01' + len(body).to_bytes(3, 'big') + body
    hello = b'\x16\x03\x03' + struct.pack('!H', len(handshake)) + handshake
    split_hello = b'\x16\x03\x03\x00\x17' + handshake[:23] + b'\x16\x03\x03' + struct.pack('!H', len(handshake) - 23) + handshake[23:]

    result = {target: {'empty': b''} for target in TARGETS}
    ipv4 = b'\x45\x00' + struct.pack('!H', 28 + len(query)) + b'\x00\x01\x00\x00\x40\x11\x00\x00\x7f\x00\x00\x01\x7f\x00\x00\x01'
    udp = struct.pack('!HHHH', 12345, 53, 8 + len(query), 0)
    result['packet'].update(dns=query, ipv4_dns=ipv4 + udp + query, http=http,
                            tls_prefix=b'\x16\x03\x03\x00\x40\x01\x00\x00\x3c')
    result['rules'].update(hosts=b'0.0.0.0 ads.example\n', suffix=b'||ads.example^', allow=b'@@||ads.example^', modifier=b'||ads.example^$badfilter', unicode='例子.example'.encode())
    result['nfnetlink'].update(ack=struct.pack('=IHHIIi', 20, 2, 0, 1, 0, 0), truncated=b'\x10\x00\x00\x00')
    for path in sorted((root / 'corpus/release_json').glob('*.json')):
        result['release_json'][path.stem] = path.read_bytes()
    result['packet'].update(tls=hello, tls_records=split_hello)
    for target, payload in [('reassembly', http), ('dns_reassembly', frame)]:
        seeds = result[target]
        seeds['complete'] = action(flags=64, payload=payload)
        for offset in (1, 2, 5, len(payload) - 1):
            first = action(payload=payload[:offset])
            second = action(sequence=100 + offset, flags=64, payload=payload[offset:])
            seeds[f'split-{offset}'] = first + second + action(op=5)
            seeds[f'out-of-order-{offset}'] = second + first + second
        seeds['wrap-syn-fin'] = action(flags=2, sequence=0xfffffffa, payload=payload[:5]) + action(sequence=0, payload=payload[5:]) + action(flags=1, sequence=len(payload) - 5)
        seeds['overlap-retransmit'] = action(payload=payload[:10]) + action(sequence=105, payload=payload[5:]) + action(payload=payload[:10])
        seeds['block-expiry-reset'] = action(payload=payload) + action(op=1) + action(payload=payload) + action(op=3, clock=6) + action(flags=2, payload=payload)
        seeds['consume-pipeline'] = action(flags=64, payload=payload + payload[:5]) + action(op=5) + action(sequence=100 + len(payload) + 5, flags=64, payload=payload[5:])
        seeds['capacity'] = b''.join(action(flow=n % 256, clock=1 + (n // 256) * 8, payload=payload[:5]) for n in range(1100))
        seeds['clock-rollback-clear'] = action(payload=payload[:5]) + action(clock=7, payload=payload[:5]) + action(op=3, clock=4) + action(op=4)
    for label, payload in [('tls', hello), ('tls-records', split_hello)]:
        result['reassembly'][label] = action(payload=payload[:7]) + action(sequence=107, payload=payload[7:31]) + action(sequence=131, payload=payload[31:])
    plain = b'one.example\r\ntwo.example\n' + b'x' * 2048 + b'\n'
    packed = gzip.compress(plain, mtime=0)
    result['rule_stream'].update(plain=b'\x00\x00' + plain, gzip=b'\x01\x00' + packed,
        truncated_gzip=b'\x01\x00' + packed[:-5], concatenated_gzip=b'\x01\x00' + packed + packed,
        inflate_limit=b'\x01\x01' + gzip.compress(b'a' * 300000, mtime=0),
        line_limit=b'\x00\x02' + b'ads.example\n' * 32, long_line=b'\x00\x04' + plain,
        gzip_required=b'\x01\x00' + plain)
    stored, deflated = module_zip(), module_zip(True)
    result['module_zip'].update(stored=stored, deflate=deflated, duplicate=module_zip(duplicate=True),
        truncated=stored[:-1], long_comment=module_zip(comment=b'x' * 65535), bad_deflate=deflated[:42] + b'\xff' + deflated[43:])
    return result
