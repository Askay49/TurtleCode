#!/usr/bin/env python3
"""Unwrap the vendored WOFF1 into an equivalent sfnt/TTF using stdlib only.

Keeps all font tables and glyph outlines; recalculates sfnt offsets, padding and
head.checkSumAdjustment. Run manually after updating the pinned Seti assets.
"""
from pathlib import Path
import struct
import zlib


def unwrap(data):
    signature, flavor, length, count, reserved, total = struct.unpack_from('>4s4sIHHI', data)
    assert signature == b'wOFF' and length == len(data) and reserved == 0
    power = 2 ** (count.bit_length() - 1)
    directory = bytearray(struct.pack('>4sHHHH', flavor, count, power * 16, power.bit_length() - 1, count * 16 - power * 16))
    tables = bytearray()
    head_offset = None
    for i in range(count):
        tag, offset, compressed, original, checksum = struct.unpack_from('>4sIIII', data, 44 + i * 20)
        table = data[offset:offset + compressed]
        if compressed < original:
            table = zlib.decompress(table)
        assert len(table) == original
        sfnt_offset = 12 + count * 16 + len(tables)
        if tag == b'head':
            head_offset = sfnt_offset
            table = table[:8] + b'\0' * 4 + table[12:]
        padded = table + b'\0' * ((-len(table)) % 4)
        assert sum(struct.unpack('>' + 'I' * (len(padded) // 4), padded)) & 0xffffffff == checksum
        directory.extend(struct.pack('>4sIII', tag, checksum, sfnt_offset, original))
        tables.extend(padded)
    sfnt = directory + tables
    assert len(sfnt) == total and head_offset is not None
    checksum = sum(struct.unpack('>' + 'I' * (len(sfnt) // 4), sfnt)) & 0xffffffff
    struct.pack_into('>I', sfnt, head_offset + 8, (0xb1b0afba - checksum) & 0xffffffff)
    return sfnt


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    (root / 'seti.ttf').write_bytes(unwrap((root / 'seti.woff').read_bytes()))
