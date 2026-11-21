#!/bin/bash
# Build the XNU loader as a freestanding AArch64 UEFI application.
set -e
cd "$(dirname "$0")"

# On macOS the system clang cannot target a PE/COFF AArch64 image, and LLVM 23 no longer
# bundles the linker: Homebrew LLVM provides clang, the separate `lld` formula provides
# lld-link. On Linux clang/lld-link are used as they are.
#   brew install llvm lld
CC=${CC:-}
LD=${LD:-}
if [ "$(uname -s)" = Darwin ]; then
	LLVM=$(brew --prefix llvm 2>/dev/null || true)
	if [ -z "$CC" ] && [ -x "$LLVM/bin/clang" ]; then
		CC="$LLVM/bin/clang"
	fi
	if [ -z "$LD" ]; then
		if [ -x "$(brew --prefix lld 2>/dev/null)/bin/lld-link" ]; then
			LD="$(brew --prefix lld)/bin/lld-link"
		else
			LD=lld-link
		fi
	fi
fi
CC=${CC:-clang}
LD=${LD:-lld-link}
if [ -n "${CELLULAR_PCIE_ACCESS_PREFLIGHT:-}" ] && [ -z "${CELLULAR_PCIE_ACCESS_RETEST:-}" ]; then
	echo 'PCIe access result inconclusive after reported battery loss; explicit checkpointed retest build required' >&2
	exit 1
fi
if [ -n "${CELLULAR_PCIE_ACCESS_PREFLIGHT:-}" ] && [ -z "${CELLULAR_PCIE_MAP_PREFLIGHT:-}" ]; then
	echo 'CELLULAR_PCIE_ACCESS_PREFLIGHT requires the isolated PCIe mapping preflight' >&2
	exit 1
fi
if [ -n "${CELLULAR_PCIE_MAP_PREFLIGHT:-}" ] && { [ -z "${CELLULAR_MMIO_PREFLIGHT:-}" ] || [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ] || [ -z "${CELLULAR_RESERVE_CP:-}" ] || [ -n "${CELLULAR_CP_BOOT_PREFLIGHT:-}" ]; }; then
	echo 'CELLULAR_PCIE_MAP_PREFLIGHT requires MMIO + fallback + reservations, and excludes powered CP BOOT' >&2
	exit 1
fi
if [ -n "${CELLULAR_PCIE_MAP_PREFLIGHT:-}" ]; then
	PCIE_MAP_DEFINE=-DCELLULAR_PCIE_MAP_PREFLIGHT=1
else
	PCIE_MAP_DEFINE=
fi
if [ -n "${CELLULAR_PCIE_ACCESS_PREFLIGHT:-}" ]; then
	PCIE_MAP_DEFINE="$PCIE_MAP_DEFINE -DCELLULAR_PCIE_ACCESS_PREFLIGHT=1"
fi
if [ -n "${CELLULAR_MMIO_PREFLIGHT:-}" ] && [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ]; then
	echo 'CELLULAR_MMIO_PREFLIGHT requires the original-loader preflight build' >&2
	exit 1
fi
if [ -n "${CELLULAR_SPI_LOOPBACK:-}" ] && { [ -z "${CELLULAR_MMIO_PREFLIGHT:-}" ] || [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ] || [ -z "${CELLULAR_RESERVE_CP:-}" ]; }; then
	echo 'CELLULAR_SPI_LOOPBACK requires MMIO + original-loader preflight + CP reservations' >&2
	exit 1
fi
if [ -n "${CELLULAR_CP_GPIO_PREFLIGHT:-}" ] && { [ -z "${CELLULAR_MMIO_PREFLIGHT:-}" ] || [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ] || [ -z "${CELLULAR_RESERVE_CP:-}" ]; }; then
	echo 'CELLULAR_CP_GPIO_PREFLIGHT requires MMIO + original-loader preflight + CP reservations' >&2
	exit 1
fi
if [ -n "${CELLULAR_CP_BOOT_PREFLIGHT:-}" ] && { [ -z "${CELLULAR_MMIO_PREFLIGHT:-}" ] || [ -z "${CELLULAR_PREFLIGHT_ONLY:-}" ] || [ -z "${CELLULAR_RESERVE_CP:-}" ] || [ -z "${CELLULAR_SPI_LOOPBACK:-}" ] || [ -z "${CELLULAR_CP_GPIO_PREFLIGHT:-}" ]; }; then
	echo 'CELLULAR_CP_BOOT_PREFLIGHT requires reserved-memory + MMIO + loopback + GPIO ownership original-loader preflights' >&2
	exit 1
fi

"$CC" $PCIE_MAP_DEFINE --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
  -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter ${LOG_IN_BOOT:+-DLOG_IN_BOOT=1} ${FLASH_BOOT_ONLY:+-DFLASH_BOOT_ONLY=1} ${CELLULAR_CP_BOOT_PREFLIGHT:+-DCELLULAR_CP_BOOT_PREFLIGHT=1} ${CELLULAR_CP_GPIO_PREFLIGHT:+-DCELLULAR_CP_GPIO_PREFLIGHT=1} ${CELLULAR_SPI_LOOPBACK:+-DCELLULAR_SPI_LOOPBACK=1} ${CELLULAR_MMIO_PREFLIGHT:+-DCELLULAR_MMIO_PREFLIGHT=1} ${CELLULAR_RESERVE_CP:+-DCELLULAR_RESERVE_CP=1} ${CELLULAR_PREFLIGHT_ONLY:+-DCELLULAR_PREFLIGHT_ONLY=1} ${GIC_V2:+-DGIC_V2=1} ${SUPER_TAIL_START:+-DSUPER_TAIL_START=${SUPER_TAIL_START}ULL} ${SUPER_ROOT_LEN:+-DSUPER_ROOT_LEN=${SUPER_ROOT_LEN}ULL} ${USERDATA_WIN_OFF:+-DUSERDATA_WIN_OFF=${USERDATA_WIN_OFF}ULL -DUSERDATA_WIN_LEN=${USERDATA_WIN_LEN}ULL} -c loader.c -o loader.obj
# /base: UEFI loads an image at its preferred address when that is free. The PE default (0x140000000)
# is in the middle of the phone's largest block of RAM, which XNU needs in one piece.
# The preflight image must leave the original loader's preferred base free.
LOADER_IMAGE_BASE=0xA1000000
if [ -n "${CELLULAR_PREFLIGHT_ONLY:-}" ]; then LOADER_IMAGE_BASE=0xA3000000; fi
"$LD" /subsystem:efi_application /entry:efi_main /nodefaultlib /base:$LOADER_IMAGE_BASE /out:loader.efi loader.obj
ls -la loader.efi
