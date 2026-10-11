#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""bin2uf2.py — превращает сырой дамп flash (2 МиБ, база 0x10000000) в .uf2 для RP2040."""
import struct, sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_FAMILY = 0x00002000
FAMILY_RP2040 = 0xE48BFF56
CHUNK = 256

def convert(src, dst, base=0x10000000):
    data = open(src, 'rb').read()
    n = (len(data) + CHUNK - 1) // CHUNK
    with open(dst, 'wb') as f:
        for i in range(n):
            chunk = data[i*CHUNK:(i+1)*CHUNK]
            chunk = chunk + b'\x00' * (CHUNK - len(chunk))
            hdr = struct.pack('<8I', MAGIC0, MAGIC1, FLAG_FAMILY, base + i*CHUNK, CHUNK, i, n, FAMILY_RP2040)
            f.write(hdr + chunk + b'\x00' * (476 - CHUNK) + struct.pack('<I', MAGIC_END))
    print(f"{src} -> {dst}: {n} блоков, {n*512} байт")

if __name__ == '__main__':
    convert(sys.argv[1] if len(sys.argv) > 1 else 'backup.bin',
            sys.argv[2] if len(sys.argv) > 2 else 'restore.uf2')
