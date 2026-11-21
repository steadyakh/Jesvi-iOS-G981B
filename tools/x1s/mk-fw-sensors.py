#!/usr/bin/env python3
"""
The sensor hub's firmware patch for the Galaxy S20 family (bcm_sensorhub_a1.bin, what bsd/dev/redwood_ssp.c loads at
shub=4..6), out of Samsung's GPL kernel header drivers/sensorhub/brcm/bbdpl/r_os/bbd_patch_file_picasso.h: the hex of
its <Blob=...> decoded, checked as the kernel checks it (records [0xF1][u16 LE length][u32 address][data] only).
The header and the .bin are Samsung's/Broadcom's: they stay out of git (fw-sensors/ is ignored).

  python tools/x1s/mk-fw-sensors.py bbd_patch_file_picasso.h fw-sensors/bcm_sensorhub_a1.bin
"""
import os, re, struct, sys

src, out = sys.argv[1], sys.argv[2]
text = open(src, encoding='latin-1').read()
# the C string pieces joined: "...\n" "..." -> one text
joined = ''.join(re.findall(r'"([^"]*)"', text)).replace(chr(92) + 'n', chr(10))
fields = dict(re.findall(r'<(\w+)=([^>]*)>', joined))
blob = bytes.fromhex(re.sub(r'\s', '', fields['Blob']))
if len(blob) != int(fields['BlobLength']):
    sys.exit('mk-fw-sensors: the blob is %d bytes, the header says %s' % (len(blob), fields['BlobLength']))
at = records = data = 0
lo, hi = 0xFFFFFFFF, 0
while at < len(blob):
    kind, length = blob[at], struct.unpack_from('<H', blob, at + 1)[0]
    if kind != 0xF1 or length < 5 or length > 1000 or at + 3 + length > len(blob):
        sys.exit('mk-fw-sensors: record %d at %d is not a write (type %#x, length %d)' % (records, at, kind, length))
    addr = struct.unpack_from('<I', blob, at + 3)[0]
    lo, hi = min(lo, addr), max(hi, addr + length - 4)
    records += 1
    data += length - 4
    at += 3 + length
os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
open(out, 'wb').write(blob)
print('mk-fw-sensors: %s: %s, ASIC %s, entry %s: %d writes, %d data bytes, %08x-%08x, %d bytes in all'
      % (out, fields.get('Name'), fields.get('AsicVersion'), fields.get('PatchAddress'), records, data, lo, hi, len(blob)))
