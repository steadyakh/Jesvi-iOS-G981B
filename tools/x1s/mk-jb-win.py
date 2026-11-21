#!/usr/bin/env python3
"""
tools/mk-jb.sh for Windows (Git Bash, no WSL): the Procursus rootless bootstrap with Sileo in it, unpacked into jb/
(never in the repository), as userland/build.sh takes it -- jb/root/var/jb -- plus what Windows cannot keep in a
directory: jb/root/var/jb.manifest, one line per entry (mode in octal with the type bits, uid, gid, path relative to
var/jb, and for a symbolic link its target), in the format of tools/apfs/mkapfs.py's manifests. userland/build.sh adds
it to the system image's manifest, so the image gets the links (1151 in the bootstrap), the owners and the set-user-ID
bits (login, su, sudo, passwd, ..., Sileo's giveMeRoot) that unpacking on NTFS loses.

  python tools/mk-jb-win.py          (downloads into jb/debs once, like mk-jb.sh)

Needs Python 3.14 (compression.zstd). The same sources and versions as mk-jb.sh.
"""
import io, os, shutil, stat, sys, tarfile, urllib.request

SUITE = '1800'
SILEO = 'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'
MIRROR = 'https://apt.procurs.us'
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'jb')
JB = os.path.join(OUT, 'root', 'var', 'jb')

try:
    from compression import zstd
except ImportError:
    sys.exit('mk-jb-win.py: needs Python 3.14 or newer (compression.zstd)')


def get(url, name):
    path = os.path.join(OUT, 'debs', name)
    if not (os.path.exists(path) and os.path.getsize(path) > 0):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with urllib.request.urlopen(url) as r, open(path + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(path + '.part', path)
    return path


def ar_members(data):
    """a .deb is an ar archive: {name: bytes}"""
    if data[:8] != b'!<arch>\n':
        sys.exit('mk-jb-win.py: the .deb is not an ar archive')
    at, out = 8, {}
    while at + 60 <= len(data):
        head = data[at:at + 60]
        name = head[:16].decode().strip().rstrip('/')
        size = int(head[48:58].decode().strip())
        out[name] = data[at + 60:at + 60 + size]
        at += 60 + size + (size & 1)
    return out


def open_tar(name, blob):
    if name.endswith('.zst'):
        return tarfile.open(fileobj=zstd.ZstdFile(io.BytesIO(blob)), mode='r|')
    return tarfile.open(fileobj=io.BytesIO(blob), mode='r:*')


def unpack(tar, manifest):
    """every entry under var/jb into JB; what NTFS cannot hold into manifest {relative path: line}"""
    for m in tar:
        name = m.name[2:] if m.name.startswith('./') else m.name
        name = name.rstrip('/')
        rel = name[len('var/jb/'):] if name.startswith('var/jb/') else None
        if not rel:
            continue
        if '..' in rel.split('/') or any(c in rel for c in ':*?"<>|\\'):
            sys.exit('mk-jb-win.py: an entry Windows cannot hold: %s' % rel)
        dest = os.path.join(JB, *rel.split('/'))
        perm = m.mode & 0o7777
        if m.isdir():
            os.makedirs(dest, exist_ok=True)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFDIR | perm, m.uid, m.gid, rel)
        elif m.issym():
            if os.path.lexists(dest) and not os.path.isdir(dest):
                os.remove(dest)
            manifest[rel] = '%o\t%d\t%d\t%s\t%s' % (stat.S_IFLNK | 0o755, m.uid, m.gid, rel, m.linkname)
        elif m.islnk():
            target = m.linkname[2:] if m.linkname.startswith('./') else m.linkname
            src = os.path.join(JB, *target[len('var/jb/'):].split('/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(src, dest)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFREG | perm, m.uid, m.gid, rel)
        elif m.isfile():
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with tar.extractfile(m) as f, open(dest, 'wb') as o:
                shutil.copyfileobj(f, o)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFREG | perm, m.uid, m.gid, rel)


def main():
    boot = get('%s/bootstraps/%s/bootstrap-iphoneos-arm64.tar.zst' % (MIRROR, SUITE), 'bootstrap.tar.zst')
    deb = get('%s/pool/main/iphoneos-arm64-rootless/%s/sileo/%s' % (MIRROR, SUITE, SILEO), 'sileo.deb')
    shutil.rmtree(os.path.join(OUT, 'root'), ignore_errors=True)
    os.makedirs(JB)
    manifest = {}
    with zstd.open(boot, 'rb') as f, tarfile.open(fileobj=f, mode='r|') as t:
        unpack(t, manifest)
    parts = ar_members(open(deb, 'rb').read())
    data = [n for n in parts if n.startswith('data.tar')]
    if not data:
        sys.exit('mk-jb-win.py: the .deb has no data.tar')
    with open_tar(data[0], parts[data[0]]) as t:
        unpack(t, manifest)
    app = os.path.join(JB, 'Applications', 'Sileo.app')
    if not os.path.isdir(app):
        sys.exit('mk-jb-win.py: no Sileo.app in the package')
    # Sileo's root helper: set-user-ID root, as the package ships it (mk-jb.sh: chmod 4755)
    rel = 'Applications/Sileo.app/giveMeRoot'
    if rel not in manifest:
        sys.exit('mk-jb-win.py: no giveMeRoot in Sileo.app')
    manifest[rel] = '%o\t0\t0\t%s' % (stat.S_IFREG | 0o4755, rel)
    with open(os.path.join(OUT, 'root', 'var', 'jb.manifest'), 'w', newline='\n') as f:
        for rel in sorted(manifest):
            f.write(manifest[rel] + '\n')
    links = sum(1 for l in manifest.values() if l.startswith('12'))
    setid = sorted(l.split('\t')[3] for l in manifest.values() if int(l.split('\t')[0], 8) & 0o6000)
    print('mk-jb-win.py: %d entries (%d links) in %s; set-ID: %s' % (len(manifest), links, JB, ', '.join(setid)))


if __name__ == '__main__':
    main()
