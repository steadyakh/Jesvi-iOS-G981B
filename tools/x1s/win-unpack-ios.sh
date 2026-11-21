#!/bin/bash
# sysimage/unpack.sh's steps the way they are done on Windows (Git Bash, no WSL): 7-Zip for the root filesystem,
# sysimage/apfs-extract.py (with zbitmap.dll and lzfse.dll from the x1s-wintools workflow) for the cryptexes, which
# Windows' 7-Zip cannot decompress. Nothing is downloaded: the IPSW is the owner's.
#
#   tools/win-unpack-sysimage.sh IPSW DLL-DIR
set -e
IPSW=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
DLLS=$(cd "$2" && pwd)
OUT=${OUT:?set OUT=the folder for the unpacked iOS files}; SRC=$(cd "$(dirname "$(readlink -f "$0")")/../../sysimage" && pwd); TOP=$(cd "$SRC/.." && pwd); mkdir -p "$OUT"; cd "$OUT"
PATH="/c/Program Files/7-Zip:$PATH"
PY=python
CACHE=20F66__iPhone10,3_6

if [ ! -f .done-rootfs ]; then
	[ -f rootfs.dmg ] || { echo "copying the root filesystem image (4.7 GB)"; $PY /e/hakios-s20/sysimage/ipsw-member.py "$IPSW" 096-04088-077.dmg rootfs.dmg.part; mv rootfs.dmg.part rootfs.dmg; }
	echo "listing the image"
	7z l -slt rootfs.dmg > rootfs.slt
	$PY "$SRC/select-system.py" rootfs.slt system.files system.manifest
	$PY "$SRC/mkvarskel.py" rootfs.slt > var.skel
	echo "extracting $(wc -l < system.files) files"
	rm -rf rootfs
	7z x -y -orootfs rootfs.dmg @system.files > extract-system.log 2>&1 || echo "7z: some files not extracted (names Windows cannot hold): see extract-system.log"
	$PY "$TOP/tools/font_trim.py" emoji rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc \
		rootfs/System/Library/Fonts/CoreAddition/AppleColorEmoji-160px.ttc 40,64,96
	$PY "$TOP/tools/font_trim.py" faces rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc \
		rootfs/System/Library/Fonts/LanguageSupport/PingFang.ttc '^\.?PingFang(SC|TC|HK)-(Regular|Semibold)$'
	touch .done-rootfs
	rm -f rootfs.dmg
fi
if [ ! -f .done-cryptex ]; then
	[ -f cryptex-os.dmg ] || { echo "copying the OS cryptex (2.7 GB)"; $PY /e/hakios-s20/sysimage/ipsw-member.py "$IPSW" 096-04666-078.dmg cryptex-os.dmg.part; mv cryptex-os.dmg.part cryptex-os.dmg; }
	rm -rf cryptex cryptex.cache $CACHE
	echo "extracting the OS cryptex"
	$PY /e/hakios-s20/sysimage/apfs-extract.py --dlls "$DLLS" --skip System/Library/Caches/com.apple.dyld --skip System/DriverKit cryptex-os.dmg cryptex
	echo "extracting the dyld shared cache"
	$PY /e/hakios-s20/sysimage/apfs-extract.py --dlls "$DLLS" --only System/Library/Caches/com.apple.dyld --skip-glob '*.symbols*' cryptex-os.dmg cryptex.cache
	mv cryptex.cache $CACHE          # (--only writes the subtree's files at the top)
	rm -rf cryptex.cache cryptex.cache.manifest
	[ "$(ls $CACHE | wc -l)" -eq 43 ] || { echo "the dyld shared cache is incomplete"; exit 1; }
	touch .done-cryptex
	rm -f cryptex-os.dmg
fi
if [ ! -f .done-cryptex-app ]; then
	[ -f cryptex-app.dmg ] || $PY /e/hakios-s20/sysimage/ipsw-member.py "$IPSW" 096-03536-083.dmg cryptex-app.dmg
	rm -rf cryptex-app
	$PY /e/hakios-s20/sysimage/apfs-extract.py --dlls "$DLLS" cryptex-app.dmg cryptex-app
	[ -f cryptex-app/usr/libexec/webbookmarksd ] || { echo "the App cryptex is incomplete"; exit 1; }
	touch .done-cryptex-app
fi
[ -f DeviceTree.d22ap.im4p ] || $PY /e/hakios-s20/sysimage/ipsw-member.py "$IPSW" Firmware/all_flash/DeviceTree.d22ap.im4p DeviceTree.d22ap.im4p
echo "done: $(du -sh rootfs | cut -f1) rootfs, $(du -sh cryptex | cut -f1) cryptex, $(du -sh cryptex-app | cut -f1) App cryptex, $(du -sh $CACHE | cut -f1) dyld cache"
