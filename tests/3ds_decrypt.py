#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""An application CIA encrypted at both layers, as a retail one is, with keys made up for the test, decrypted by the
driver (snx_ctr_decrypt) and extracted: its extraction is the plain CIA's. The layers as 3dbrew documents them:
each content AES-CBC under the ticket's title key (itself AES-CBC under the common key), and the NCCH's AES-CTR, the
ExHeader, ExeFS header, icon and banner under the primary key (slot 0x2C) and the rest under the secondary one the
crypto method names, every normal key the key scrambler of a KeyX and KeyY."""
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

from ctr_fixtures import TITLE, cia, ncch, put

try:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
except ImportError:
    print('cryptography (the Python package) is not installed: skipped')
    sys.exit(77)

MASK = (1 << 128) - 1


def rol(x, n):
    return ((x << n) | (x >> (128 - n))) & MASK


def scramble(x, y, c):
    x, y, c = (int.from_bytes(k, 'big') for k in (x, y, c))
    return rol(((rol(x, 2) ^ y) + c) & MASK, 87).to_bytes(16, 'big')


def ctr(key, counter, data, offset):
    start = (int.from_bytes(counter, 'big') + offset // 16) & MASK
    e = Cipher(algorithms.AES(key), modes.CTR(start.to_bytes(16, 'big'))).encryptor()
    pad = offset % 16
    return e.update(bytes(pad) + bytes(data))[pad:]


def cbc(key, iv, data):
    e = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return e.update(bytes(data)) + e.finalize()


KEYS = {name: hashlib.sha256(name.encode()).digest()[:16] for name in ('gen', 'x2c', 'x25', 'x3d', 'common0', 'title')}


def keys_file(path, common0=KEYS['common0']):
    path.write_text(
        ':AES\n# made up for the test\n'
        f'generatorConstant={KEYS["gen"].hex().upper()}\n'
        f'slot0x2CKeyX={KEYS["x2c"].hex().upper()}\nslot0x25KeyX={KEYS["x25"].hex().upper()}\n'
        f'slot0x3DKeyX={KEYS["x3d"].hex().upper()}\ncommon0={common0.hex().upper()}\n'
        ':RSA\nsecureInfoExp=010001\n'
    )


def encrypt_ncch(plain):
    """the NCCH's layer, crypto method 1 (secondary key slot 0x25), version 2 counters"""
    n = bytearray(plain)
    n[0:16] = hashlib.sha256(b'key y').digest()[:16]  # the signature, whose first 16 bytes are the KeyY
    put(n, 0x108, '<Q', TITLE)  # the partition ID
    n[0x18B] = 1
    n[0x18F] &= ~4  # no longer NoCrypto
    primary = scramble(KEYS['x2c'], n[0:16], KEYS['gen'])
    secondary = scramble(KEYS['x25'], n[0:16], KEYS['gen'])

    def counter(kind):
        return n[0x108:0x110][::-1] + bytes([kind]) + bytes(7)

    out = bytearray(n)
    out[0x200:0xA00] = ctr(primary, counter(1), n[0x200:0xA00], 0)
    exefs, exefs_size = (struct.unpack_from('<I', n, o)[0] * 512 for o in (0x1A0, 0x1A4))
    header = n[exefs : exefs + 0x200]
    out[exefs : exefs + 0x200] = ctr(primary, counter(2), header, 0)
    for i in range(10):
        name = header[i * 16 : i * 16 + 8].rstrip(b'\0')
        at, size = struct.unpack_from('<II', header, i * 16 + 8)
        if not name or not size:
            continue
        key = primary if name in (b'icon', b'banner') else secondary
        lo = exefs + 0x200 + at
        out[lo : lo + size] = ctr(key, counter(2), n[lo : lo + size], 0x200 + at)
    romfs, romfs_size = (struct.unpack_from('<I', n, o)[0] * 512 for o in (0x1B0, 0x1B4))
    if romfs_size:
        out[romfs : romfs + romfs_size] = ctr(secondary, counter(3), n[romfs : romfs + romfs_size], 0)
    return bytes(out)


def encrypt_cia(blob):
    """the CIA's layer over every present content, and the title key in the ticket under common key 0"""
    b = bytearray(blob)
    header, cert, tik, tmd, meta, contents = struct.unpack_from('<IHHIIIIQ', b, 0)[0:1] + struct.unpack_from(
        '<IIIIQ', b, 8
    )
    align = lambda x: (x + 63) & ~63
    tik_at = align(0x2040 + cert)
    tmd_at = align(tik_at + tik)
    at = align(tmd_at + tmd)
    common = scramble(KEYS['x3d'], KEYS['common0'], KEYS['gen'])
    body = tik_at + 0x140
    b[body + 0x7F : body + 0x8F] = cbc(common, struct.pack('>Q', TITLE) + bytes(8), KEYS['title'])
    b[body + 0xB1] = 0
    tmd_body = tmd_at + 0x140
    for i in range(struct.unpack_from('>H', b, tmd_body + 0x9E)[0]):
        chunk = tmd_body + 0x9C4 + i * 0x30
        index = struct.unpack_from('>H', b, chunk + 4)[0]
        if not b[0x20 + index // 8] & (0x80 >> (index % 8)):
            continue
        size = struct.unpack_from('>Q', b, chunk + 8)[0]
        b[at : at + size] = cbc(KEYS['title'], struct.pack('>H', index) + bytes(14), b[at : at + size])
        at += size
    return bytes(b)


def present_encrypted(chunks):
    for i in range(2):  # the manual (index 0) and the application (5): both present, both encrypted
        struct.pack_into('>H', chunks, i * 0x30 + 6, 1)


def tree(folder):
    return {p.relative_to(folder).as_posix(): p.read_bytes() for p in sorted(folder.rglob('*')) if p.is_file()}


def main():
    driver = os.path.abspath(sys.argv[1])  # not resolved: scarletnx-3ds may be a link to scarletnx, named so
    with tempfile.TemporaryDirectory(prefix='snx-ctr-decrypt-') as temp:
        root = Path(temp)
        env = {**os.environ, 'HOME': str(root), 'USERPROFILE': str(root)}

        def run(name, blob, keys, status, message):
            source = root / f'{name}.cia'
            source.write_bytes(blob)
            args = [driver, '-x', '--output', str(root / name)] + (['--keys', str(keys)] if keys else []) + [str(source)]
            p = subprocess.run(args, capture_output=True, text=True, cwd=root, timeout=60, env=env)
            text = p.stdout + p.stderr
            assert p.returncode == status and message in text, (name, p.returncode, text)
            assert 'AddressSanitizer' not in text and 'runtime error:' not in text, text
            return root / name

        application = ncch()
        plain = run('plain', cia(application=application), None, 0, 'Extracted')
        encrypted = encrypt_cia(cia(application=encrypt_ncch(application), patch_chunks=present_encrypted))
        keys = root / 'aes_keys.txt'
        keys_file(keys)
        decrypted = run('encrypted', encrypted, keys, 0, 'Decrypted')
        a, b = tree(plain), tree(decrypted)
        for name in ('exheader.bin', 'exefs/.code', 'exefs/icon'):
            assert a[name] == b[name], name
        assert {k: v for k, v in a.items() if k.startswith('romfs/')} == {
            k: v for k, v in b.items() if k.startswith('romfs/')
        }
        decrypted_cia = (root / 'encrypted.decrypted.cia').read_bytes()
        assert len(decrypted_cia) == len(encrypted)
        # The decrypted copy is taken as it is the next time, not decrypted again
        assert 'Using the decrypted copy' in subprocess.run(
            [driver, '-x', '--keys', str(keys), '--output', str(root / 'encrypted'), str(root / 'encrypted.cia')],
            capture_output=True, text=True, cwd=root, timeout=60, env=env,
        ).stdout
        # A wrong common key decrypts to a title key that makes nonsense of every content, which the checks refuse
        wrong = root / 'wrong_keys.txt'
        keys_file(wrong, common0=bytes(16))
        p = subprocess.run(
            [driver, '-x', '--keys', str(wrong), '--output', str(root / 'wrong'), str(root / 'encrypted.cia')],
            capture_output=True, text=True, cwd=root, timeout=60, env=env,
        )
        assert p.returncode and not (root / 'wrong').exists(), p.stdout + p.stderr
        # Without keys the driver says where it looked
        run('nokeys', encrypted, None, 3, 'no 3DS keys')
    print('CTR CIA and NCCH decryption: both layers taken off with synthetic keys, extraction equal to the plain one')


if __name__ == '__main__':
    main()
