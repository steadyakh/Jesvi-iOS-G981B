#!/bin/bash
# The G981B port's iOS images on the owner's Windows PC (Git Bash, no WSL): userland/root.apfs (our programs and the
# launchd jobs) and userland/system.apfs (iOS 16.5 from the owner's own IPSW), as the earlier HakiOS-S20 port's
# devices/x1s/build-images.sh made them.
#
#   tools/x1s/build-images-win.sh PROGRAMS-DIR IOS-DIR [VAR=value ...]
#
# PROGRAMS-DIR: prebuilt/userland plus the GL libraries from gpu-userland-x1s.yml (libOSMesa.8.dylib, libredwood-gl).
# IOS-DIR: the iOS files unpacked by tools/x1s/win-unpack-ios.sh. Nothing here talks to a phone.
set -e
cd "$(dirname "$(readlink -f "$0")")/../.."
P=${1:?usage: tools/x1s/build-images-win.sh PROGRAMS-DIR IOS-DIR [VAR=value ...]}
I=${2:?usage: tools/x1s/build-images-win.sh PROGRAMS-DIR IOS-DIR [VAR=value ...]}
shift 2
[ -f "$P/stage0" ] && [ -f "$P/init" ] && [ -f "$P/libOSMesa.8.dylib" ] ||
	{ echo "build-images-win: $P has no stage0, init or libOSMesa.8.dylib" >&2; exit 1; }
for d in rootfs cryptex cryptex-app 20F66__iPhone10,3_6; do
	[ -d "$I/$d" ] || { echo "build-images-win: $I/$d is missing (tools/x1s/win-unpack-ios.sh)" >&2; exit 1; }
done
# "python3" in Git Bash is the Microsoft Store's stub: a python3 that runs the real python
shim=$(mktemp -d)
printf '#!/bin/sh\nexec python "$@"\n' > "$shim/python3"
chmod +x "$shim/python3"
trap 'rm -rf "$shim"' EXIT
[ "$(PATH="$shim:$PATH" python3 -c 'print(1)')" = 1 ] || { echo "build-images-win: python does not run" >&2; exit 1; }
env PATH="$shim:$PATH" PREBUILT=1 PREBUILT_DIR="$(cd "$P" && pwd)" IOS_INPUT_DIR="$(cd "$I" && pwd)" "$@" bash ${TRACE:+-x} userland/build.sh
ls -la userland/root.apfs userland/system.apfs
