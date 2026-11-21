#!/usr/bin/env python3
"""
tools/mk-jb.sh for Windows (Git Bash, no WSL): the Procursus rootless bootstrap with Sileo in it, unpacked into jb/
(never in the repository), as userland/build.sh takes it -- jb/root/var/jb -- plus what Windows cannot keep in a
directory: jb/root/var/jb.manifest, one line per entry (mode in octal with the type bits, uid, gid, path relative to
var/jb, and for a symbolic link its target), in the format of tools/apfs/mkapfs.py's manifests. userland/build.sh adds
it to the system image's manifest, so the image gets the links (1151 in the bootstrap), the owners and the set-user-ID
bits (login, su, sudo, passwd, ..., Sileo's giveMeRoot) that unpacking on NTFS loses.

  python tools/x1s/mk-jb-win.py          (downloads into jb/debs once, with curl, like mk-jb.sh)

Every step of mk-jb.sh is done here too, in the same order: Sileo and ElleKit written into dpkg's database, newer
versions of a few packages (PREINSTALL), apt's sources and certificate file, the three packages that only break apt
taken out (shshd, libkrw0, libdimentio0), the dependency on the "firmware" pseudo-package taken out, and prep.sh.
Without them Sileo reports "Depends firmware", "essential Depends org.coolstar.sileo | ..." and "libkrw0 Depends
libkrw0-plugin" and refuses to install anything.

Needs Python 3.14 (compression.zstd) and curl.
"""
import importlib.util, io, os, re, shutil, stat, subprocess, sys, tarfile

SUITE = '1800'
SILEO = 'org.coolstar.sileo_2.5.1_iphoneos-arm64.deb'
ELLEKIT = 'ellekit_0.4.5_iphoneos-arm64.deb'
MIRROR = 'https://apt.procurs.us'
# mk-jb.sh's PREINSTALL less OpenSSH (nothing on this port starts it, and there is no key for it)
PREINSTALL = ['debianutils', 'launchctl', 'preferenceloader', 'libssl3', 'libmd0']
# mk-jb.sh: they read and write the kernel of a jailbroken phone through a plug-in nothing here provides
DROP = ['shshd', 'libkrw0', 'libdimentio0']
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
OUT = os.path.join(ROOT, 'jb')
JB = os.path.join(OUT, 'root', 'var', 'jb')
STATUS = os.path.join(JB, 'Library', 'dpkg', 'status')

try:
    from compression import zstd
except ImportError:
    sys.exit('mk-jb-win.py: needs Python 3.14 or newer (compression.zstd)')

# Debian version order and the stanza helpers, from Jesvi's own tools/jb-preinstall.py
_spec = importlib.util.spec_from_file_location('jb_preinstall', os.path.join(ROOT, 'tools', 'jb-preinstall.py'))
jbp = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(jbp)


def get(url, name):
    """fetched once into jb/debs, with curl as mk-jb.sh does (the repository turns Python's urllib away)"""
    path = os.path.join(OUT, 'debs', name)
    if not (os.path.exists(path) and os.path.getsize(path) > 0):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if subprocess.run(['curl', '-sL', '--fail', '-o', path + '.part', url]).returncode != 0:
            sys.exit('mk-jb-win.py: cannot fetch %s' % url)
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


def deb_tar(path, kind):
    """the data or control tar of a .deb"""
    parts = ar_members(open(path, 'rb').read())
    names = [n for n in parts if n.startswith(kind + '.tar')]
    if not names:
        sys.exit('mk-jb-win.py: %s has no %s.tar' % (path, kind))
    return open_tar(names[0], parts[names[0]])


def control_of(path):
    """the control file of a .deb, as text"""
    with deb_tar(path, 'control') as t:
        for m in t:
            if m.name.lstrip('./') == 'control' and m.isfile():
                return t.extractfile(m).read().decode('utf-8')
    sys.exit('mk-jb-win.py: %s has no control file' % path)


def remove_path(rel):
    dest = os.path.join(JB, *rel.split('/'))
    if os.path.isdir(dest) and not os.path.islink(dest):
        shutil.rmtree(dest)
    elif os.path.lexists(dest):
        os.remove(dest)


def unpack(tar, manifest):
    """every entry under var/jb into JB; what NTFS cannot hold into manifest {relative path: line}.
    A later package's entry replaces an earlier one's (an upgrade)."""
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
            if os.path.lexists(dest) and not os.path.isdir(dest):
                os.remove(dest)
            os.makedirs(dest, exist_ok=True)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFDIR | perm, m.uid, m.gid, rel)
        elif m.issym():
            if os.path.lexists(dest):
                remove_path(rel)
            manifest[rel] = '%o\t%d\t%d\t%s\t%s' % (stat.S_IFLNK | 0o755, m.uid, m.gid, rel, m.linkname)
        elif m.islnk():
            target = m.linkname[2:] if m.linkname.startswith('./') else m.linkname
            src = os.path.join(JB, *target[len('var/jb/'):].split('/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copyfile(src, dest)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFREG | perm, m.uid, m.gid, rel)
        elif m.isfile():
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            with tar.extractfile(m) as f, open(dest, 'wb') as o:
                shutil.copyfileobj(f, o)
            manifest[rel] = '%o\t%d\t%d\t%s' % (stat.S_IFREG | perm, m.uid, m.gid, rel)


def write_file(manifest, rel, text, mode=0o644):
    """a file of our own in the prefix, owned by root, with its directories"""
    dest = os.path.join(JB, *rel.split('/'))
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    parts = rel.split('/')
    for i in range(1, len(parts)):
        d = '/'.join(parts[:i])
        manifest.setdefault(d, '%o\t0\t0\t%s' % (stat.S_IFDIR | 0o755, d))
    manifest[rel] = '%o\t0\t0\t%s' % (stat.S_IFREG | mode, rel)


def read_status():
    return list(jbp.stanzas(open(STATUS, encoding='utf-8', errors='replace').read()))


def write_status(stanzas):
    with open(STATUS, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n\n'.join(stanzas) + '\n\n')


def installed(control):
    """a control file as a dpkg status record"""
    return '\n'.join(l for l in control.strip('\n').split('\n') if l.strip()) + '\nStatus: install ok installed'


def put_status(stanzas, record):
    """record in place of the one of the same package, or added"""
    name = jbp.field(record, 'Package')
    for i, s in enumerate(stanzas):
        if jbp.field(s, 'Package') == name:
            stanzas[i] = record
            return
    stanzas.append(record)


PREP = r'''#!/var/jb/bin/sh
# Run once per boot, after the prefix has been grafted onto /var/jb (tools/mk-jb.sh's prep.sh, for the G981B).
#
# The prefix lives on the data volume (it outlives a boot), and init keeps what is there: dpkg's database from a
# boot before this image is kept, too. An older image (21 Nov 2026) put in a database without Sileo, ElleKit and the
# fixes of tools/mk-jb.sh, and in it Sileo can install nothing ("Depends firmware", "essential Depends
# org.coolstar.sileo | ..."). Such a database is replaced, once, with this image's; the old one is kept beside it.
db=/var/jb/Library/dpkg/status
if [ -f /var/jb/.image-status ] && ! /var/jb/usr/bin/grep -q '^Package: org.coolstar.sileo$' "$db"; then
	/var/jb/usr/bin/cp "$db" "$db.before-x1s-fix"
	/var/jb/usr/bin/cp /var/jb/.image-status "$db"
	echo "prep.sh: dpkg's database replaced with this image's (the old one: $db.before-x1s-fix)"
fi
/var/jb/usr/libexec/firmware
for app in /var/jb/Applications/*.app; do
	[ -d "/Applications/${app##*/}" ] && continue
	real=$(/var/jb/usr/bin/readlink "$app/Info.plist" 2>/dev/null)
	real=${real%/Info.plist}
	(
		for try in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24; do
			for path in "$app" ${real:+"$real"}; do
				case "$(/var/jb/usr/bin/uicache -p "$path" 2>&1)" in
				*Error*) ;;
				*) echo "prep.sh: ${app##*/} registered from $path (try $try)"; exit 0 ;;
				esac
			done
			sleep 5
		done
		echo "prep.sh: ${app##*/} could not be registered"
	) &
done
/var/jb/usr/sbin/pwd_mkdb -p /var/jb/etc/master.passwd 2>/dev/null
for package in debianutils apt dash bash zsh vi; do
	[ -x "/var/jb/Library/dpkg/info/$package.postinst" ] &&
		"/var/jb/Library/dpkg/info/$package.postinst" configure 99999 >/dev/null 2>&1
done
exit 0
'''


def main():
    boot = get('%s/bootstraps/%s/bootstrap-iphoneos-arm64.tar.zst' % (MIRROR, SUITE), 'bootstrap.tar.zst')
    sileo = get('%s/pool/main/iphoneos-arm64-rootless/%s/sileo/%s' % (MIRROR, SUITE, SILEO), 'sileo.deb')
    ellekit = get('https://ellekit.space/pool/%s' % ELLEKIT, 'ellekit.deb')
    index = get('%s/dists/iphoneos-arm64-rootless/%s/main/binary-iphoneos-arm64/Packages' % (MIRROR, SUITE),
                'Packages')
    shutil.rmtree(os.path.join(OUT, 'root'), ignore_errors=True)
    os.makedirs(JB)
    manifest = {}
    with zstd.open(boot, 'rb') as f, tarfile.open(fileobj=f, mode='r|') as t:
        unpack(t, manifest)

    # Sileo, and dpkg told it is installed (or Sileo offers to install itself, and "essential" is unmet)
    with deb_tar(sileo, 'data') as t:
        unpack(t, manifest)
    if not os.path.isdir(os.path.join(JB, 'Applications', 'Sileo.app')):
        sys.exit('mk-jb-win.py: no Sileo.app in the package')
    rel = 'Applications/Sileo.app/giveMeRoot'
    if rel not in manifest:
        sys.exit('mk-jb-win.py: no giveMeRoot in Sileo.app')
    manifest[rel] = '%o\t0\t0\t%s' % (stat.S_IFREG | 0o4755, rel)
    status = read_status()
    put_status(status, installed(control_of(sileo)))

    # ElleKit: the tweak loader; it Provides mobilesubstrate, which nearly every tweak depends on
    with deb_tar(ellekit, 'data') as t:
        unpack(t, manifest)
    for d in ('usr/lib/TweakInject', 'Library/MobileSubstrate'):
        os.makedirs(os.path.join(JB, *d.split('/')), exist_ok=True)
        manifest.setdefault(d, '%o\t0\t0\t%s' % (stat.S_IFDIR | 0o755, d))
    rel = 'Library/MobileSubstrate/DynamicLibraries'
    remove_path(rel)
    manifest[rel] = '%o\t0\t0\t%s\t%s' % (stat.S_IFLNK | 0o755, rel, '/var/jb/usr/lib/TweakInject')
    put_status(status, installed(control_of(ellekit)))

    # newer versions of what the bootstrap carries (tools/jb-preinstall.py's choice, unpacked here)
    best = {}
    for s in jbp.stanzas(open(index, encoding='utf-8', errors='replace').read()):
        name = jbp.field(s, 'Package')
        if name in PREINSTALL and (name not in best or
                                   jbp.newer(jbp.field(s, 'Version') or '0', jbp.field(best[name], 'Version') or '0')):
            best[name] = s
    have = {jbp.field(s, 'Package'): jbp.field(s, 'Version') for s in status}
    done = []
    for name in PREINSTALL:
        if name not in best:
            print('mk-jb-win.py: %s is not in the repository' % name)
            continue
        s = best[name]
        version = jbp.field(s, 'Version')
        if have.get(name) is not None and not jbp.newer(version, have[name]):
            continue
        filename = jbp.field(s, 'Filename')
        deb = get('%s/%s' % (MIRROR, filename), os.path.basename(filename).replace(':', '_'))
        with deb_tar(deb, 'data') as t:
            unpack(t, manifest)
        put_status(status, '\n'.join(l for l in s.split('\n') if not l.startswith(
            ('Filename:', 'Size:', 'MD5sum:', 'SHA1:', 'SHA256:', 'SHA512:', 'Description-md5:')))
            + '\nStatus: install ok installed')
        done.append('%s %s%s' % (name, version, ' (was %s)' % have[name] if have.get(name) else ''))
    # a package that still writes to Substrate's old directory: the link back (as jb-preinstall.py does)
    old_dir, new_dir = 'Library/MobileSubstrate/DynamicLibraries', 'usr/lib/TweakInject'
    if manifest.get(old_dir, '').split('\t')[0] != '%o' % (stat.S_IFLNK | 0o755):
        for rel in sorted(r for r in manifest if r.startswith(old_dir + '/')):
            moved = new_dir + rel[len(old_dir):]
            fields = manifest.pop(rel).split('\t')
            fields[3] = moved
            manifest[moved] = '\t'.join(fields)
            src = os.path.join(JB, *rel.split('/'))
            if os.path.isfile(src):
                os.makedirs(os.path.dirname(os.path.join(JB, *moved.split('/'))), exist_ok=True)
                os.replace(src, os.path.join(JB, *moved.split('/')))
        remove_path(old_dir)
        manifest[old_dir] = '%o\t0\t0\t%s\t%s' % (stat.S_IFLNK | 0o755, old_dir, '/var/jb/' + new_dir)
        print('mk-jb-win.py: the tweaks moved to %s, the old directory a link again' % new_dir)

    # the packages that only break apt, files and all
    info = os.path.join(JB, 'Library', 'dpkg', 'info')
    for package in DROP:
        lst = os.path.join(info, package + '.list')
        if os.path.exists(lst):
            for path in open(lst, encoding='utf-8').read().split('\n'):
                rel = path.strip().lstrip('/')
                rel = rel[len('var/jb/'):] if rel.startswith('var/jb/') else rel
                if rel and rel in manifest and not manifest[rel].startswith('4'):
                    remove_path(rel)
                    del manifest[rel]
        for f in os.listdir(info):
            if f.startswith(package + '.'):
                os.remove(os.path.join(info, f))
                manifest.pop('Library/dpkg/info/' + f, None)
    status = [s for s in status if jbp.field(s, 'Package') not in DROP]

    # the dependency on "firmware": no repository has that package, and Sileo's own index calls it not installable
    changed = 0
    for i, s in enumerate(status):
        lines = s.split('\n')
        for k, line in enumerate(lines):
            if not line.startswith('Depends: '):
                continue
            groups = line[len('Depends: '):].split(', ')
            keep = [g for g in groups if not re.match(r'^\s*firmware\s*[( ]|^\s*firmware$', g.split(' | ')[0])]
            if len(keep) != len(groups):
                lines[k] = 'Depends: ' + ', '.join(keep) if keep else None
                changed += 1
        status[i] = '\n'.join(l for l in lines if l is not None)
    write_status(status)

    # apt: Procursus as the first repository, and the certificates read from the prefix's own file
    write_file(manifest, 'etc/apt/sources.list.d/procursus.sources',
               'Types: deb\nURIs: %s\nSuites: iphoneos-arm64-rootless/%s\nComponents: main\n' % (MIRROR, SUITE))
    write_file(manifest, 'etc/apt/apt.conf.d/10redwood.conf',
               'Acquire::https::CaInfo "/var/jb/etc/ssl/certs/cacert.pem";\n')

    # prep.sh, and this image's database beside it for prep.sh to put in place of a broken one on the phone
    write_file(manifest, 'prep.sh', PREP, 0o755)
    shutil.copyfile(STATUS, os.path.join(JB, '.image-status'))
    manifest['.image-status'] = '%o\t0\t0\t.image-status' % (stat.S_IFREG | 0o644)
    remove_path('prep_bootstrap.sh')
    manifest.pop('prep_bootstrap.sh', None)

    # the manifest's files must all be there
    for rel, line in manifest.items():
        if line.startswith('10') and not os.path.isfile(os.path.join(JB, *rel.split('/'))):
            sys.exit('mk-jb-win.py: %s is in the manifest but not on disk' % rel)
    with open(os.path.join(OUT, 'root', 'var', 'jb.manifest'), 'w', newline='\n') as f:
        for rel in sorted(manifest):
            f.write(manifest[rel] + '\n')
    links = sum(1 for l in manifest.values() if l.startswith('12'))
    setid = sorted(l.split('\t')[3] for l in manifest.values() if int(l.split('\t')[0], 8) & 0o6000)
    print('mk-jb-win.py: %d entries (%d links) in %s; set-ID: %s' % (len(manifest), links, JB, ', '.join(setid)))
    print('mk-jb-win.py: %d packages in dpkg\'s database; "firmware" taken out of %d; dropped %s; newer: %s'
          % (len(status), changed, ', '.join(DROP), ', '.join(done) or 'none'))


if __name__ == '__main__':
    main()
