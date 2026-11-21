#!/bin/bash
# Source-only build. No phone, private firmware, prebuilt fallback or source export.
set -euo pipefail
cd "$(dirname "$0")/.."
test "$(uname -s)" = Darwin || { echo 'release kernel CI currently requires macOS' >&2; exit 1; }
jobs=${1:-4}
case "$jobs" in ''|*[!0-9]*) exit 2 ;; esac
device=${2:-x1slte}
case "$device" in
    x1slte) out=out/kernel ;;
    y2s|z3s) out=out/experimental/"$device" ;;
    *) echo "unsupported kernel target: $device" >&2; exit 2 ;;
esac
mkdir -p "$out"
# Keep the release source tree separate from an agent's hand-edited xnu-src.
# Use the exported overlay, and never export back into it from this build.
source_key=$(python3 tools/release/source_key.py)
export XNU_SRC="$PWD/.cache/release-kernel/$source_key"
test -d "$XNU_SRC" || bash xnu/apply.sh "$XNU_SRC"
bash xnu-deps/setup.sh --stage1
bash xnu-deps/setup.sh --rebuild-libs
export GIC_V2=1
bash xnu/xnu-build.sh -j"$jobs" -k build > "$out/build.log" 2>&1 || {
    grep -n -E 'error:|fatal error' "$out/build.log" | head -40 >&2 || true
    tail -60 "$out/build.log" >&2
    exit 1
}
kernel=$XNU_SRC/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
test -s "$kernel"
/usr/bin/strings "$kernel" | /usr/bin/grep 'gic: v2' > /dev/null
cp "$kernel" "$out/mach.$device"
/usr/bin/xcrun strip -x "$out/mach.$device"
test "$(wc -c < "$out/mach.$device")" -le 16777216
test "$(python3 tools/release/source_key.py)" = "$source_key" || {
    echo 'source inputs changed during the build; refusing stale provenance' >&2
    exit 1
}
python3 tools/release/kernel_manifest.py "$device" "$out"
