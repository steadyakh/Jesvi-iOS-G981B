#!/usr/bin/env python3
"""Pack a boot-partition image for the Galaxy S20 5G (SM-G981B, x1s): Mu-Silicium's x1sPkg UEFI, the Jesvi iOS loader
in its payload, and optionally the kernel and the root image, laid out to fit the 59 MiB boot partition.

usage: pack-boot.py --uefi Mu-x1s.img --stock-boot boot.img --loader loader.efi [--kernel mach.x1slte]
                    [--root root.apfs] [--cmdline "..."] [--devmode] -o hakios-x1s-jesvi.img

Layout (all inside `boot`, the only partition the owner allows to be written; recovery = TWRP is never touched):
  0x0000000  the UEFI image (Android boot image v2: shim + FD), the XNU payload at FD + 0x100000 (file 0x100870)
  0x1000000  the loader's log slot, 8 MiB (xnuloader LOG_IN_BOOT=1; built with FLASH_BOOT_ONLY=1 too)
  0x1800000  the kernel, up to 20 MiB              (payload: kpart=boot koff=1800000)
  0x2C00000  root.apfs, up to the partition's end  (payload: rpart=boot roff=2c00000 rsize=...)
The loader only READS the kernel and the root from boot. The DTB section is the phone's own Samsung DT table, taken
from its stock boot image (as the earlier port's working images did; S-Boot rejects a bare FDT). No AVB footer: the
earlier port's boot images had none and S-Boot started them. Nothing here writes to a phone.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import tempfile

BOOT_PARTITION = 0x3B00000          # 59 MiB, the G981B's boot partition (its stock image is exactly this long)
PAYLOAD_AT = 0x100870               # FD at page + 112 = 0x870; payload at FD + 0x100000
PAYLOAD_MAX = 0x80000
LOG_AT, KERNEL_AT, ROOT_AT = 0x1000000, 0x1800000, 0x2C00000
HERE = os.path.dirname(os.path.abspath(__file__))
MKPAYLOAD = os.path.join(HERE, '..', '..', 'xnuloader', 'mkpayload.py')


def hdr(d):
    """Android boot image v2 header fields."""
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ks, rs, ss = (struct.unpack_from('<I', d, o)[0] for o in (8, 16, 24))
    ps = struct.unpack_from('<I', d, 36)[0]
    ver = struct.unpack_from('<I', d, 40)[0]
    rec = struct.unpack_from('<I', d, 1632)[0]
    dsz = struct.unpack_from('<I', d, 1648)[0]
    return dict(ks=ks, rs=rs, ss=ss, ps=ps, ver=ver, rec=rec, dsz=dsz)


def sections(h):
    pg = lambda n: (n + h['ps'] - 1) // h['ps'] * h['ps']
    koff = h['ps']
    roff = koff + pg(h['ks'])
    soff = roff + pg(h['rs'])
    recoff = soff + pg(h['ss'])
    doff = recoff + pg(h['rec'])
    return pg, koff, roff, soff, recoff, doff


def stock_dt_table(path):
    d = open(path, 'rb').read()
    h = hdr(d)
    assert h['ver'] == 2, 'the stock boot image is not header v2'
    _, _, _, _, _, doff = sections(h)
    dt = d[doff:doff + h['dsz']]
    assert dt[:4] == bytes.fromhex('d7b7ab1e'), 'the stock DTB section is not a Samsung DT table'
    return dt


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--uefi', required=True)
    ap.add_argument('--stock-boot', required=True, help="the phone's own stock boot.img (its backup), for the DT table")
    ap.add_argument('--loader', required=True)
    ap.add_argument('--kernel')
    ap.add_argument('--root')
    ap.add_argument('--cmdline', default='')
    ap.add_argument('--rdpart', help='a partition the loader READS the system image from into memory (e.g. super)')
    ap.add_argument('--rdsize', type=lambda x: int(x, 0), help='the size of the system image in bytes (it starts at 0)')
    ap.add_argument('--devmode', action='store_true', help="the loader's menu and text at every start")
    ap.add_argument('-o', '--output', required=True)
    a = ap.parse_args()

    img = bytearray(open(a.uefi, 'rb').read())
    h = hdr(img)
    assert h['ps'] == 2048 and h['ver'] == 2, 'the UEFI image is not a page-2048 v2 boot image'
    assert img[0x870 + 0x28:0x870 + 0x2c] == b'_FVH', 'no firmware volume at page + 112'
    slot = img[PAYLOAD_AT:PAYLOAD_AT + PAYLOAD_MAX]
    assert slot == b'\xff' * PAYLOAD_MAX or slot[:4] == b'XNUP', 'the payload slot (FD + 0x100000) is in use'

    ker = open(a.kernel, 'rb').read() if a.kernel else b''
    root = open(a.root, 'rb').read() if a.root else b''
    assert len(ker) <= ROOT_AT - KERNEL_AT, 'kernel > 20 MiB'
    assert ROOT_AT + len(root) <= BOOT_PARTITION, 'root.apfs does not fit in the boot partition'

    with tempfile.TemporaryDirectory() as t:
        payload = os.path.join(t, 'payload.bin')
        cmd = [sys.executable, MKPAYLOAD, '-o', payload, 'loader.efi=' + a.loader]
        if a.devmode:
            cmd.append('--devmode')
        if ker:
            cmd += ['--kpart', 'boot', '--koff', '%x' % KERNEL_AT]
        if root:
            cmd += ['--rpart', 'boot', '--roff', '%x' % ROOT_AT, '--rsize', '%x' % len(root)]
        if a.rdpart:
            cmd += ['--rdpart', a.rdpart, '--rdoff', '0', '--rdsize', '%x' % a.rdsize]
        if a.cmdline:
            cmd += ['--cmdline', a.cmdline]
        subprocess.run(cmd, check=True)
        p = open(payload, 'rb').read()
    assert len(p) <= PAYLOAD_MAX, 'payload too large'
    img[PAYLOAD_AT:PAYLOAD_AT + PAYLOAD_MAX] = b'\xff' * PAYLOAD_MAX
    img[PAYLOAD_AT:PAYLOAD_AT + len(p)] = p

    # the header's OS version / patch level and command line as the phone's stock image has them: the earlier port's
    # images (devices/x1s/mkbootimg.py there) kept them and S-Boot started them; Mu-Silicium leaves both empty
    stock = open(a.stock_boot, 'rb').read(2048)
    img[44:48] = stock[44:48]           # os_version (OS version and security patch level)
    img[64:64 + 512] = stock[64:64 + 512]  # cmdline
    img[608:608 + 1024] = stock[608:608 + 1024]  # extra_cmdline
    # the DTB section: the phone's own Samsung DT table, header size/address/id updated (as x1slte.sh pack does)
    dt = stock_dt_table(a.stock_boot)
    pg, koff, roff, soff, recoff, doff = sections(h)
    end = doff + pg(h['dsz'])
    if len(img) < doff + pg(len(dt)):
        img.extend(bytes(doff + pg(len(dt)) - len(img)))
    img[doff:end] = bytes(end - doff)
    img[doff:doff + len(dt)] = dt
    struct.pack_into('<I', img, 1648, len(dt))
    struct.pack_into('<Q', img, 1652, 0x10000000)
    sha = hashlib.sha1()
    for off, n in ((koff, h['ks']), (roff, h['rs']), (soff, h['ss']), (recoff, h['rec']), (doff, len(dt))):
        sha.update(bytes(img[off:off + n]))
        sha.update(struct.pack('<I', n))
    img[576:576 + 32] = sha.digest().ljust(32, b'\0')
    head_end = doff + pg(len(dt))
    assert head_end <= LOG_AT, 'the UEFI image runs into the log slot'

    out = bytearray(img[:head_end])
    size = max(len(out), KERNEL_AT + len(ker) if ker else 0, ROOT_AT + len(root) if root else 0)
    out.extend(bytes(size - len(out)))
    if ker:
        out[KERNEL_AT:KERNEL_AT + len(ker)] = ker
    if root:
        out[ROOT_AT:ROOT_AT + len(root)] = root
    assert len(out) <= BOOT_PARTITION
    open(a.output, 'wb').write(out)
    print('pack-boot: %s, %d bytes (boot partition %d); UEFI head %d, payload %d, kernel %d, root %d; sha256 %s'
          % (a.output, len(out), BOOT_PARTITION, head_end, len(p), len(ker), len(root),
             hashlib.sha256(out).hexdigest()))


if __name__ == '__main__':
    main()
