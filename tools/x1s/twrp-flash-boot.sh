#!/bin/bash
# Write an image to the G981B's BOOT partition from TWRP (adb), the only partition the owner allows, and read it back.
#
#   tools/x1s/twrp-flash-boot.sh IMAGE
#
# Refuses unless: the phone is in TWRP, the image fits the 59 MiB boot partition, the copy on the phone has the same
# SHA-256 as the image (both non-empty). Afterwards the first IMAGE-size bytes of boot must read back the same.
# (On Windows' Git Bash adb paths like /tmp are rewritten unless MSYS_NO_PATHCONV=1: set here for adb only.)
set -euo pipefail
export PATH=/c/platform-tools-latest-windows/platform-tools:$PATH
img=${1:?usage: twrp-flash-boot.sh IMAGE}
[ -f "$img" ] || { echo "twrp-flash-boot: no $img" >&2; exit 1; }
n=$(wc -c < "$img")
[ "$n" -gt 0 ] && [ "$n" -le 61865984 ] || { echo "twrp-flash-boot: $n bytes does not fit the boot partition" >&2; exit 1; }
want=$(sha256sum "$img" | cut -c1-64)
a() { MSYS_NO_PATHCONV=1 adb "$@"; }
state=$(a get-state 2>/dev/null | tr -d '\r' || true)     # (not `| grep -q`: with pipefail, adb's broken pipe fails it)
[ "$state" = recovery ] || { echo "twrp-flash-boot: the phone is not in TWRP ($state)" >&2; exit 1; }
a push "$(cygpath -w "$img")" /tmp/flash-boot.img > /dev/null
got=$(a shell "sha256sum /tmp/flash-boot.img" | cut -c1-64)
[ ${#want} -eq 64 ] && [ "$got" = "$want" ] || { echo "twrp-flash-boot: copy on the phone differs ($got)" >&2; exit 1; }
a shell "dd if=/tmp/flash-boot.img of=/dev/block/by-name/boot bs=1048576 conv=fsync 2>/dev/null; sync"
back=$(a shell "head -c $n /dev/block/by-name/boot | sha256sum" | cut -c1-64)
a shell "rm -f /tmp/flash-boot.img"
[ "$back" = "$want" ] || { echo "twrp-flash-boot: READ BACK DIFFERENT ($back); restore a backup" >&2; exit 1; }
echo "twrp-flash-boot: $img ($n bytes, sha256 $want) written to boot and read back"
