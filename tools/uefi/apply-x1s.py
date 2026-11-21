#!/usr/bin/env python3
"""Apply the SM-G981B (x1s) overlay to a Mu-Silicium checkout, for Jesvi iOS on the Galaxy S20 5G.

usage: apply-x1s.py MU_SILICIUM_DIR

Mu-Silicium's own x1sPkg is the UEFI for this phone. Three changes, the same ones Jesvi iOS makes for the S20 LTE
(devices/exynos990/y2s/legacy/uefi-y2s, which is a renamed x1sPkg):
  1. BootManagerLib: start the XNU loader carried in the payload (XnuLoader.c/.h from devices/exynos990/x1s/uefi;
     this copy writes no storage at all and never resets the phone by itself).
  2. x1sPkg's memory map: the modem (S5123 CP) regions reserved, at the same addresses as on the G980F; read from
     this phone's own device tree in the earlier port (cp-rmem@f0000000, cp-rmem2@d0000000, cp-msi@f6200000).
  3. Nothing else: x1sPkg already has this phone's 12 GB layout (EL2 early memory at 0xAFE800000).
Each edit checks that the text it changes is there exactly once, so a changed upstream fails loudly.
"""
import shutil
import sys
from pathlib import Path

here = Path(__file__).resolve().parents[2]
overlay = here / 'devices/exynos990/x1s/uefi'
mu = Path(sys.argv[1])


def edit(path, old, new):
    text = path.read_text()
    if text.count(old) != 1:
        sys.exit(f'apply-x1s: {path}: expected exactly one "{old.strip()[:60]}", found {text.count(old)}')
    path.write_text(text.replace(old, new))
    print(f'apply-x1s: edited {path.relative_to(mu)}')


bm = mu / 'Silicon/Silicium/SiliciumPkg/Library/BootManagerLib'
for name in ('XnuLoader.c', 'XnuLoader.h'):
    shutil.copy(overlay / name, bm / name)
    print(f'apply-x1s: copied {name}')
edit(bm / 'BootManager.c', '#include "BootManager.h"\n', '#include "BootManager.h"\n#include "XnuLoader.h"\n')
edit(bm / 'BootManager.c', '  DeviceBootManagerUnableToBoot ();\n',
     '  DeviceBootManagerUnableToBoot ();\n\n'
     '  // Jesvi iOS (x1s): hand over to the XNU loader payload (does not return if it starts).\n'
     '  XnuLoaderMain ();\n')
edit(bm / 'BootManagerLib.inf', '  BootManager.c\n', '  BootManager.c\n  XnuLoader.c\n')
edit(bm / 'BootManagerLib.inf', '  gEfiSimpleTextInputExProtocolGuid\n',
     '  gEfiSimpleTextInputExProtocolGuid\n  gEfiBlockIoProtocolGuid\n  gEfiPartitionInfoProtocolGuid\n')

mm = mu / 'Platforms/Samsung/x1sPkg/Library/MemoryMapLib/MemoryMapLib.c'
el2 = '  {"EL2 Early Memory",   0xAFE800000,'
edit(mm, el2,
     '  // Jesvi iOS (x1s): the modem (S5123 CP) regions of this phone\'s device tree, kept out of every pool.\n'
     '  {"CP IPC",             0xF0000000, 0x00800000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},\n'
     '  {"CP MSI",             0xF6200000, 0x00001000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},\n'
     '  {"CP Packet Memory",   0xD0000000, 0x04000000, AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, UNCACHED_UNBUFFERED},\n'
     + el2)
print('apply-x1s: done')
