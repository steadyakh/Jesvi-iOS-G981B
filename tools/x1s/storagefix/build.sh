#!/bin/bash
# X1SStorageFix.dylib for Settings (see X1SStorageFix.c), with the Windows LLVM (LLVM=dir of clang.exe and lld.exe).
set -e
cd "$(dirname "$0")"
B=${LLVM:-/e/s20-work/llvm/bin}
"$B/clang.exe" -target arm64-apple-ios14.0 -O2 -fno-stack-protector -ffreestanding -c X1SStorageFix.c -o X1SStorageFix.o
"$B/lld.exe" -flavor darwin -arch arm64 -platform_version ios 14.0 16.5 -no_fixup_chains -dylib libSystem.tbd libobjc.tbd \
	-install_name /var/jb/usr/lib/TweakInject/X1SStorageFix.dylib -adhoc_codesign X1SStorageFix.o -o X1SStorageFix.dylib
rm -f X1SStorageFix.o
echo "storagefix: $(pwd)/X1SStorageFix.dylib"
