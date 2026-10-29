#!/usr/bin/env python3
# Makes the zips in packaging/wasm/test/fixtures/zips, which unzip-test.mjs
# opens with unzip.js: entries made by the real tools of each compression
# method, so the decoders are checked against those, not against themselves.
#
#   python3 make-zips.py <hwzip-2.4/src> <out dir>
#
# Needs 7-Zip (7z), xz, and the Python packages zstandard, pyppmd and
# inflate64. The PKZIP 1.x entries are test data from hwzip 2.4 by Hans
# Wennborg (https://www.hanshq.net/zip.html, public domain): its tests hold
# them as C arrays, which this copies out. Its hamlet.txt (Project Gutenberg's
# Hamlet) is the text the other entries are made from.
#
#   pkzip-multi.zip    PKZIP 0.92, 1.10 and 2.50: store, shrink, reduce 1-4,
#                      implode (both kinds) and deflate (hwzip's multizip)
#   pkzip-implode.zip  implode with each window and tree, as PKZIP 1.01 and
#                      1.10 wrote it (hwzip's legacy_implode_zip)
#   pkzip-streams.zip  PKZIP's reduce, implode and shrink streams from
#                      hwzip's tests, in entries of their own
#   shrink.zip         shrink with partial clears (hwzip's shrinker, which
#                      matches PKZIP's, ported below)
#   7zip.zip           7-Zip: Deflate, Deflate64, BZip2, LZMA and PPMd
#   python.zip         Python's zlib, bz2 and lzma (zip LZMA and .xz), the xz
#                      tool, zstandard, pyppmd (7-Zip's PPMd) and inflate64

import bz2
import lzma
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib

import inflate64
import pyppmd
import zstandard

HW, OUT = sys.argv[1], sys.argv[2]
os.makedirs(OUT, exist_ok=True)


def c_array(path, name):
    src = open(os.path.join(HW, path)).read()
    m = re.search(r'uint8_t ' + name + r'\[\] = \{(.*?)\};', src, re.S)
    return bytes(int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]{2}', m.group(1)))


hamlet = open(os.path.join(HW, 'hamlet.txt'), 'rb').read()


def x86ish(n):
    # Text with x86 calls and jumps through it, for xz's x86 filter, and runs
    # of the bytes it looks at, for the cases of calls close together.
    r = random.Random(2)
    out = bytearray()
    while len(out) < n:
        k = r.random()
        if k < 0.65:
            s = r.randrange(len(hamlet) - 20)
            out += hamlet[s:s + r.randrange(1, 20)]
        elif k < 0.95:
            out.append(r.choice([0xe8, 0xe9]))
            out += (r.randrange(-0x8000, 0x8000) & 0xffffffff).to_bytes(4, 'little')
        else:
            out += bytes(r.choice([0xe8, 0xe9, 0x00, 0xff, 0x41]) for _ in range(r.randrange(1, 13)))
    return bytes(out[:n])


DATA = {
    'text.txt': hamlet[:24000],
    'hamlet-120k.txt': hamlet[:120000],
    'rep.txt': hamlet[:20000] * 120,
    'far.txt': hamlet[:36000] * 2,
    'zeros.bin': bytes(100000),
    'random.bin': random.Random(1).randbytes(4096),
    'x86.bin': x86ish(30000),
    'one.txt': b'a',
    'empty.txt': b'',
}


def zip_of(entries):
    # entries: (name, method, flags, compressed, crc, size)
    out, cd = bytearray(), bytearray()
    for name, method, flags, comp, crc, size in entries:
        n = name.encode()
        hdr = struct.pack('<HHHHHIIIHH', 63, flags, method, 0, 0x21, crc, len(comp), size, len(n), 0)
        cd += struct.pack('<I', 0x02014b50) + struct.pack('<H', 63) + hdr
        cd += struct.pack('<HHHII', 0, 0, 0, 0, len(out)) + n
        out += struct.pack('<I', 0x04034b50) + hdr + n + comp
    eocd = struct.pack('<IHHHHIIH', 0x06054b50, 0, 0, len(entries), len(entries), len(cd), len(out), 0)
    return bytes(out + cd + eocd)


def entries_of(path, prefix):
    # Every entry of a zip, raw.
    res = []
    blob = open(path, 'rb').read()
    for i in zipfile.ZipFile(path).infolist():
        n, e = struct.unpack_from('<HH', blob, i.header_offset + 26)
        at = i.header_offset + 30 + n + e
        res.append((prefix + os.path.basename(i.filename), i.compress_type, i.flag_bits,
                    blob[at:at + i.compress_size], i.CRC, i.file_size))
    return res


def entry(name, method, comp, data, flags=0):
    return (name, method, flags, comp, zlib.crc32(data), len(data))


def write(name, entries):
    with open(os.path.join(OUT, name), 'wb') as f:
        f.write(zip_of(entries))


# ---- PKZIP, from hwzip.

open(os.path.join(OUT, 'pkzip-multi.zip'), 'wb').write(c_array('zip_test.c', 'multizip'))
open(os.path.join(OUT, 'pkzip-implode.zip'), 'wb').write(c_array('zip_test.c', 'legacy_implode_zip'))
write('pkzip-streams.zip', [
    entry('shrink/ababcbababaaaaaaa', 1, c_array('shrink_test.c', 'lzw_fig5_shrunk'), b'ababcbababaaaaaaa'),
    entry('reduce4/hamlet-2048', 5, c_array('reduce_test.c', 'hamlet_2048'), hamlet[:2048]),
    entry('reduce4/hamlet-2048-zeros-1024', 5, c_array('reduce_test.c', 'zeros_reduced'),
          hamlet[:2048] + bytes(1024)),
    entry('implode-4k-2trees/hamlet-256', 6, c_array('implode_test.c', 'hamlet_256'), hamlet[:256], 0),
    entry('implode-8k-3trees/hamlet-8192', 6, c_array('implode_test.c', 'hamlet_8192'), hamlet[:8192], 6),
])


# ---- Shrink: hwzip's hwshrink (shrink.c), ported.

def shrink(src):
    out = bytearray()
    bits = [0, 0]  # buffer, count

    def put(v, n):
        bits[0] |= v << bits[1]
        bits[1] += n
        while bits[1] >= 8:
            out.append(bits[0] & 0xff)
            bits[0] >>= 8
            bits[1] -= 8

    table = {}  # (prefix, byte) -> code
    queue = list(range(257, 8192))
    size = [9]

    def code(c):
        while c > (1 << size[0]) - 1:
            put(256, size[0])
            put(1, size[0])
            size[0] += 1
        put(c, size[0])

    if not src:
        return b''
    cur = src[0]
    for b in src[1:]:
        nxt = table.get((cur, b))
        if nxt is not None:
            cur = nxt
            continue
        code(cur)
        if not queue:
            prefixes = {p for (p, _) in table}
            table = {k: v for k, v in table.items() if v in prefixes}
            queue = [c for c in range(257, 8192) if c not in prefixes]
            put(256, size[0])
            put(2, size[0])
        if queue:
            table[(cur, b)] = queue.pop(0)
        cur = b
    code(cur)
    if bits[1]:
        out.append(bits[0] & 0xff)
    return bytes(out)


assert shrink(b'ababcbababaaaaaaa') == c_array('shrink_test.c', 'lzw_fig5_shrunk')
write('shrink.zip', [
    entry('shrink/hamlet-60k.txt', 1, shrink(hamlet[:60000]), hamlet[:60000]),
    entry('shrink/zeros.bin', 1, shrink(DATA['zeros.bin']), DATA['zeros.bin']),
    entry('shrink/x86.bin', 1, shrink(DATA['x86.bin']), DATA['x86.bin']),
    entry('shrink/one.txt', 1, shrink(b'a'), b'a'),
])


# ---- 7-Zip.

def sevenzip(tag, names, *opts):
    d = tempfile.mkdtemp()
    for n in names:
        open(os.path.join(d, n), 'wb').write(DATA[n])
    z = os.path.join(d, 'out.zip')
    subprocess.run(['7z', 'a', '-tzip', z, *names, *opts, '-bso0', '-bsp0'], cwd=d, check=True)
    res = entries_of(z, '7zip-' + tag + '/')
    shutil.rmtree(d)
    return res


sz = []
sz += sevenzip('deflate', ['text.txt', 'random.bin'], '-mm=Deflate', '-mx=9')
sz += sevenzip('deflate64', ['far.txt', 'zeros.bin', 'random.bin', 'x86.bin', 'one.txt'], '-mm=Deflate64', '-mx=9')
sz += sevenzip('bzip2-d100k', ['hamlet-120k.txt', 'zeros.bin', 'random.bin', 'one.txt'], '-mm=BZip2', '-md=100k', '-mmt=off')
sz += sevenzip('bzip2-mt', ['text.txt'], '-mm=BZip2', '-mx=9', '-mmt=4')
sz += sevenzip('lzma', ['text.txt', 'zeros.bin', 'random.bin', 'x86.bin', 'one.txt'], '-mm=LZMA', '-mx=9')
sz += sevenzip('lzma-d64k-lc0-lp2-pb0', ['rep.txt'], '-mm=LZMA', '-md=64k', '-mlc=0', '-mlp=2', '-mpb=0')
sz += sevenzip('lzma-lc8', ['x86.bin'], '-mm=LZMA', '-mlc=8', '-mpb=4')
sz += sevenzip('ppmd-o6-restart', ['text.txt', 'random.bin', 'one.txt'], '-mm=PPMd', '-mmem=1m', '-mo=6', '-mx=5')
sz += sevenzip('ppmd-o16-cutoff', ['hamlet-120k.txt', 'x86.bin'], '-mm=PPMd', '-mmem=1m', '-mo=16', '-mx=9')
sz += sevenzip('ppmd-o2', ['text.txt'], '-mm=PPMd', '-mmem=1m', '-mo=2')
sz += sevenzip('ppmd-o12-2m-cutoff', ['text.txt'], '-mm=PPMd', '-mmem=2m', '-mo=12', '-mx=9')
write('7zip.zip', sz)


# ---- Python's libraries and the xz tool.

def raw_deflate(data, level=9, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 9, strategy)
    return c.compress(data) + c.flush()


def deflate64(data):
    d = inflate64.Deflater()
    return d.deflate(data) + d.flush()


def zip_lzma(data):
    # Zip's LZMA: version 9.20, the size of the properties, then the
    # properties and the stream, which ends with the end marker (flag bit 1).
    c = lzma.LZMACompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA1, 'preset': 6}])
    props = lzma._encode_filter_properties({'id': lzma.FILTER_LZMA1, 'preset': 6})
    return bytes([9, 20]) + struct.pack('<H', len(props)) + props + c.compress(data) + c.flush()


def xz(data, **kw):
    return lzma.compress(data, format=lzma.FORMAT_XZ, **kw)


def xz_tool(data, *opts):
    return subprocess.run(['xz', '-c', *opts], input=data, stdout=subprocess.PIPE, check=True).stdout


def crc32le(b):
    return struct.pack('<I', zlib.crc32(b))


def xz_lzma2_chunks(data):
    # LZMA2 chunks of every kind, which xz only writes for bigger data than
    # belongs here: uncompressed chunks with and without a dictionary reset,
    # and LZMA ones that reset the state with and without new properties.
    # The LZMA ones are compressed on their own (with nothing before them
    # to match) and follow bytes that leave them decoding the same: a 0
    # before them, at a position a multiple of 4 (pb is 2).
    def lzma_chunk(part, control):
        c = lzma.LZMACompressor(lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA2, 'preset': 6,
                                                           'dict_size': 1 << 20}])
        raw = c.compress(part) + c.flush()
        # One chunk: control, unpacked size, packed size, properties, data;
        # then the end.
        assert raw[0] == 0xe0 and (raw[1] << 8 | raw[2]) + 1 == len(part)
        assert len(raw) == 6 + (raw[3] << 8 | raw[4]) + 1 + 1 and raw[-1] == 0
        return bytes([control]) + raw[1:5] + (raw[5:6] if control >= 0xc0 else b'') + raw[6:-1]

    def stored(part, control):
        return bytes([control]) + struct.pack('>H', len(part) - 1) + part

    rnd = random.Random(3)
    a = rnd.randbytes(4095) + b'\0'
    b = data[:6000] + b'\0' * 4
    c = rnd.randbytes(999) + b'\0'
    d = data[6000:12000]
    plain = a + b + c + d
    assert len(a) % 4 == 0 and len(b) % 4 == 0 and len(c) % 4 == 0
    chunks = stored(a, 1) + lzma_chunk(b, 0xc0) + stored(c, 2) + lzma_chunk(d, 0xa0) + b'\0'
    assert lzma.decompress(chunks, lzma.FORMAT_RAW, filters=[{'id': lzma.FILTER_LZMA2, 'dict_size': 1 << 20}]) == plain

    # The .xz container around them, with a CRC-32 check.
    flags = b'\0\x01'
    head = b'\xfd7zXZ\0' + flags + crc32le(flags)
    bh = bytes([2, 0, 0x21, 1, 16, 0, 0, 0])
    bh += crc32le(bh)
    block = bh + chunks + bytes(-len(chunks) % 4) + crc32le(plain)
    unpadded = len(bh) + len(chunks) + 4
    def vli(v):
        o = bytearray()
        while v >= 0x80:
            o.append(v & 0x7f | 0x80)
            v >>= 7
        return bytes(o + bytes([v]))
    index = b'\0' + vli(1) + vli(unpadded) + vli(len(plain))
    index += bytes(-len(index) % 4)
    index += crc32le(index)
    back = struct.pack('<I', len(index) // 4 - 1) + flags
    out = head + block + index + crc32le(back) + back + b'YZ'
    assert lzma.decompress(out) == plain
    return out, plain


def zstd(data, **kw):
    return zstandard.ZstdCompressor(**kw).compress(data)


def zstd_stream(data, **kw):
    c = zstandard.ZstdCompressor(**kw).compressobj()
    return c.compress(data) + c.flush()


def ppmd(data, order, mb, restore):
    e = pyppmd.Ppmd8Encoder(order, mb << 20, restore)
    head = struct.pack('<H', (order - 1) | ((mb - 1) << 4) | (restore << 12))
    return head + e.encode(data) + e.flush(endmark=True)


D = DATA
py = []
py.append(entry('zlib-1/text.txt', 8, raw_deflate(D['text.txt'], 1), D['text.txt']))
py.append(entry('zlib-fixed/text.txt', 8, raw_deflate(D['text.txt'], 9, zlib.Z_FIXED), D['text.txt']))
py.append(entry('zlib/random-stored.bin', 8, raw_deflate(D['random.bin'], 0), D['random.bin']))
py.append(entry('zlib/empty.txt', 8, raw_deflate(b''), b''))
for n in ['text.txt', 'zeros.bin', 'random.bin', 'empty.txt']:
    py.append(entry('inflate64/' + n, 9, deflate64(D[n]), D[n]))
py.append(entry('bz2-level1/hamlet-120k.txt', 12, bz2.compress(D['hamlet-120k.txt'], 1), D['hamlet-120k.txt']))
py.append(entry('bz2/x86.bin', 12, bz2.compress(D['x86.bin']), D['x86.bin']))
py.append(entry('bz2/empty.txt', 12, bz2.compress(b''), b''))
py.append(entry('bz2-two-streams/text.txt', 12,
                bz2.compress(D['text.txt'][:20000]) + bz2.compress(D['text.txt'][20000:]),
                D['text.txt']))
for n in ['text.txt', 'x86.bin', 'one.txt', 'empty.txt']:
    py.append(entry('lzma/' + n, 14, zip_lzma(D[n]), D[n], 2))
py.append(entry('xz/text.txt', 95, xz(D['text.txt']), D['text.txt']))
py.append(entry('xz-9e-sha256/text.txt', 95, xz(D['text.txt'], preset=9 | lzma.PRESET_EXTREME,
                                                    check=lzma.CHECK_SHA256), D['text.txt']))
py.append(entry('xz-1-crc32/rep.txt', 95, xz(D['rep.txt'], preset=1, check=lzma.CHECK_CRC32),
                D['rep.txt']))
py.append(entry('xz-none/random.bin', 95, xz(D['random.bin'], check=lzma.CHECK_NONE), D['random.bin']))
py.append(entry('xz-x86/x86.bin', 95, xz(D['x86.bin'], filters=[{'id': lzma.FILTER_X86},
                                                              {'id': lzma.FILTER_LZMA2, 'preset': 6}]),
                D['x86.bin']))
py.append(entry('xz-delta4-lc0-lp2-pb0/x86.bin', 95,
                xz(D['x86.bin'], filters=[{'id': lzma.FILTER_DELTA, 'dist': 4},
                                          {'id': lzma.FILTER_LZMA2, 'preset': 6, 'lc': 0, 'lp': 2, 'pb': 0}]),
                D['x86.bin']))
py.append(entry('xz/empty.txt', 95, xz(b''), b''))
py.append(entry('xz-lzma2-chunks/text-and-random.bin', 95, *xz_lzma2_chunks(hamlet)))
py.append(entry('xz-two-streams-padded/text.txt', 95,
                xz(D['text.txt'][:1000]) + bytes(8) + xz(D['text.txt'][1000:]),
                D['text.txt']))
py.append(entry('xz-tool-blocks/text.txt', 95, xz_tool(D['text.txt'], '--block-size=65536'), D['text.txt']))
py.append(entry('xz-tool-threads/rep.txt', 95, xz_tool(D['rep.txt'], '-T4', '--block-size=1MiB'),
                D['rep.txt']))
py.append(entry('zstd-1/rep.txt', 93, zstd(D['rep.txt'], level=1), D['rep.txt']))
py.append(entry('zstd-3-checksum/text.txt', 93, zstd(D['text.txt'], level=3, write_checksum=True),
                D['text.txt']))
py.append(entry('zstd-19/text.txt', 93, zstd(D['text.txt'], level=19), D['text.txt']))
py.append(entry('zstd-22/x86.bin', 93, zstd(D['x86.bin'], level=22), D['x86.bin']))
py.append(entry('zstd-stream/text.txt', 93, zstd_stream(D['text.txt']), D['text.txt']))
py.append(entry('zstd/zeros.bin', 93, zstd(D['zeros.bin']), D['zeros.bin']))
py.append(entry('zstd/random.bin', 93, zstd(D['random.bin']), D['random.bin']))
py.append(entry('zstd/empty.txt', 93, zstd(b''), b''))
py.append(entry('zstd-frames-skippable/text.txt', 93,
                zstd(D['text.txt'][:20000]) + struct.pack('<II', 0x184d2a50, 4) + b'skip'
                + zstd_stream(D['text.txt'][20000:]), D['text.txt']))
py.append(entry('pyppmd-o8-1m-restart/hamlet-120k.txt', 98, ppmd(D['hamlet-120k.txt'], 8, 1, 0), D['hamlet-120k.txt']))
py.append(entry('pyppmd-o16-1m-cutoff/x86.bin', 98, ppmd(D['x86.bin'], 16, 1, 1), D['x86.bin']))
py.append(entry('pyppmd-o4-16m/zeros.bin', 98, ppmd(D['zeros.bin'], 4, 16, 0), D['zeros.bin']))
py.append(entry('pyppmd-o6/empty.txt', 98, ppmd(b'', 6, 1, 0), b''))
write('python.zip', py)

for n in sorted(os.listdir(OUT)):
    print(n, os.path.getsize(os.path.join(OUT, n)))
