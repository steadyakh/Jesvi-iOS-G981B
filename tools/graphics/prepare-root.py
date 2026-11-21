#!/usr/bin/env python3
"""Stage shared graphics support into an existing root; preserve unrelated files."""
import argparse
from pathlib import Path
import shutil
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('tree',type=Path);p.add_argument('--binaries',type=Path,required=True)
a=p.parse_args();repo=Path(__file__).resolve().parents[2]
files={'RedwoodMetal':'lib/RedwoodMetal.bundle/RedwoodMetal',
       'libredwoodair.dylib':'lib/libredwoodair.dylib',
       'libredwood-graphics.dylib':'lib/libredwood-graphics.dylib',
       'metal_props':'bin/metal_props'}
for source in files:
 if not (a.binaries/source).is_file():raise SystemExit('Missing graphics binary: '+source)
rc=a.tree/'etc/rc';old=rc.read_text()
if not any(line.startswith('exec ') for line in old.splitlines()):raise SystemExit('No init handoff in rc')
legacy_hook=a.tree/'lib/libredwood-metaloff.dylib'
if legacy_hook.is_file() or legacy_hook.is_symlink():legacy_hook.unlink()
for source,dest in files.items():
 target=a.tree/dest;target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(a.binaries/source,target)
shutil.copy2(repo/'userland/src/metal_plugin.plist',a.tree/'lib/RedwoodMetal.bundle/Info.plist')
link=a.tree/'lib/RedwoodMetal.bundle/libredwoodair.dylib'
if link.is_symlink():link.unlink()
if not link.exists():
 import os
 if os.name=='nt' and os.environ.get('MKAPFS_LINKS'):
  # Windows (the G981B port's image build): no symbolic links without a privilege; noted for mkapfs.py instead
  with open(os.environ['MKAPFS_LINKS'],'a',newline='\n') as notes:notes.write(f'{link.resolve()}\t../libredwoodair.dylib\n')
 else:link.symlink_to('../libredwoodair.dylib')
# Publish the real device once, before normal applications start. metal=1 remains
# the kernel prerequisite; the existing softkeybag/launchd handoff is unchanged.
# Old kernels must retain discovery off; auto verifies the kernel policy before
# enabling. Remove the rejected global-on rollout when updating an existing root.
old='\n'.join(line for line in old.splitlines() if line.strip() != '/redwood/bin/metal_props on')+'\n'
rc.write_text(old)
command='/redwood/bin/metal_props auto'
if command not in old.splitlines():
 lines=old.splitlines();at=next(i for i,l in enumerate(lines) if l.startswith('exec '));lines.insert(at,command);rc.write_text('\n'.join(lines)+'\n')
conf=a.tree/'etc/inject.conf';rules=[]
for rule in conf.read_text().splitlines():
 if not rule.strip():continue
 path=rule.split()[0]
 if any(s in path for s in ['libredwood-maps-metalon','maps-layer-rasterize','libredwood-graphics','libredwood-metaloff','libredwood-metalon']):continue
 rules.append(rule)
rules.extend([
 '/redwood/lib/libredwood-graphics.dylib /Applications/ /private/var/containers/Bundle/Application/ /var/containers/Bundle/Application/ /System/Library/ExtensionKit/Extensions/',
])
conf.write_text('\n'.join(rules)+'\n')
print('Prepared paired driver, guarded discovery and shared presentation; core compatibility belongs to the kernel')
