# Jesvi iOS on the Galaxy S20 5G (SM-G981B, x1s): port plan

Fork of [jesvi-eulah/Jesvi-iOS](https://github.com/jesvi-eulah/Jesvi-iOS) (credit: Jesvi; see README and NOTICE).
Started 21 November 2026. Status: **planning; nothing has run on a G981B.**

## Why this should work

The G981B and the G980F (the only phone Jesvi iOS supports) share the Exynos 990, the Mali-G77, the 1440x3200
s6e3hab panel, sec_ts touch, BCM4375 Wi-Fi/Bluetooth, MAX77705 charger, CS40L25 haptics, S2MPB02 torch and the S20
camera set. The differences:

| | SM-G980F (x1slte) | SM-G981B (x1s) |
|---|---|---|
| RAM | 8 GB | 12 GB (EL2 early memory 0xAFE800000 per Mu-Silicium x1sPkg, not 0x9FEC00000) |
| Modem | LTE | 5G: different CP memory map/firmware; cellular is expected to fail at first |
| UEFI | jesvi-eulah g980f-uefi (x1sltePkg) | Mu-Silicium **x1sPkg** (made for this phone) |
| Storage | measured by Jesvi | measured in the earlier HakiOS-S20 port: USERDATA 116,253,523,968 bytes at LBA 2850304, PIT-matched |
| Bootloader | G980FXXSNHYB7 | the owner's G981B bootloader (to be read from the phone, read-only) |

## Owner's rules (from the earlier port, still in force)

- Only the **boot** partition is written for tests (not recovery, param, up_param, efs, sec_efs, vbmeta, bootloader).
  Jesvi's installer writes recovery, super, userdata and the power-on pictures: it is **not** used on this phone.
  Its image goes into `boot` instead (Jesvi's own "standalone" mode does the same), flashed by the owner in TWRP.
- Ask before any write to the phone's storage or any power/clock change. Heat guard on; Mongoose cores off.
- No Apple or Samsung files in git or in public build artifacts.
- First tests run from RAM where possible; never start a test straight out of TWRP (GPU state: black screen).

## Status (21 Nov 2026)

- Built on this fork's public CI: Jesvi's kernel (unchanged, 44902222478) and with `thermal_stop` (44903962682);
  the x1s UEFI (44902857873, Mu-Silicium 5cbdfb2a). Loader built on Windows with `LOG_IN_BOOT=1 FLASH_BOOT_ONLY=1`.
- **On the phone:** step 1 (UEFI + loader menu, no kernel, `--devmode`) written to `boot` from TWRP and read back
  (sha256 d73be559...). Previous boot backed up first: `E:\s20-g981b\boot-backups\boot-before-jesvi-step1.img`
  (sha256 a97e41bc..., matched the phone). Restarted 12:24: nothing on USB (the loader starts USB only from its menu).
  What the screen shows is not known yet.
- Ready, not flashed: step 2 = step 1 + the guarded kernel, boot line `-v cpumask=0x3f dvfs=1 thermal_stop=70`
  (Mongoose cores off, clocks read only, power off at 70 C). No root image yet: the kernel is expected to stop at
  the root mount; the goal is to see it start.
- Undo: flash the backup above to `boot` in TWRP.
- **Measured on the phone (12:35-12:55):** step 1 reached the loader's menu (owner saw it). "Log access (USB)"
  works on Windows through libusb (`tools/x1s/fb.py`; Windows binds the loader's 18d1:d00d to WinUSB, fastboot.exe
  cannot see it). The loader's memory map shows all 12 GB (bank 0x880000000-0xB00000000) and the three modem regions
  reserved; its 4 KB log write to boot+16 MiB read back correct.
- **DO NOT flash through the loader on this phone.** `oem write boot 0` of 2.4 MB failed ("the write failed") and
  left the first 512 KB of boot as zeros (Mu-Silicium x1sPkg's UFS write through UEFI Block I/O; cause not found).
  Afterwards the loader's reads timed out. `oem recovery-mode` (a PMU reboot reason, no storage write) brought the
  phone to TWRP, where step 2 was written with adb `dd` and read back (sha256 bd31d15c...). Flash only from TWRP:
  `tools/x1s/twrp-flash-boot.sh IMAGE` (checks, writes boot only, reads back).
- **Measured on the phone (12:50): Jesvi's kernel boots on the G981B.** 6 cores (Mongoose off by `cpumask=0x3f`),
  bsd_init, IOKit, then "Waiting on ... Apple_HFS" and after 60 s `panic: Failed to mount root device` (expected: no
  root in the image). It returned to the loader, which kept the log (`oem lastlog` + upload; E:\s20-g981b\logs\
  jesvi-step2-kernel.txt). The kernel sees about 6 GB (the loader's MAX_MEM_SIZE), not 12.
- Found in that run: the heat guard did not run (the ACPM thread starts only after the root is mounted). Fixed: with
  `thermal_stop` the thread starts 1 s after EARLY_BOOT (CI 44906251526).
- **Measured on the phone (13:00): the heat power-off works.** With `thermal_stop=20` at a phone of about 25 C the
  phone switched itself off about 15 s after the kernel started, twice (the owner pressed Power once in between).
- Now in boot: the same with `thermal_stop=70` (sha256 2a1dec51...).

- **Measured on the phone (21 Nov, 18:25): Jesvi iOS runs on the G981B -- home screen, touch works.** super holds
  Jesvi's system.apfs (2,592,194,560 bytes, sha256 d9684f08..., owner's OK; the earlier port's image is backed up as
  E:\s20-g981builds\system-jb-20261114.apfs). Boot: UEFI + loader + kernel + root (11.7 MB at boot+44 MiB) and
  `rdpart=super` (the loader reads the system volume into RAM; nothing is written at run time). Boot line:
  `cpumask=0x3f dvfs=1 thermal_stop=70 mali=1 metal=1 camtl=1 fb_threads=6 touch_ts=1 jit_flush=1`. GPU about 20
  frames/s, 35-44 C. Fixed on the way: CR LF line ends from Windows in /etc/rc (metal_props never published Metal).

## Next

- The iOS images (root.apfs and the system volume) from the owner's IPSW, built on Windows. Read in the source
  (21 Nov): `userland/build.sh` takes our programs from `prebuilt/userland` when there is no ld64 (PREBUILT=1; 49
  programs are tracked there, among them stage0, init, libredwood.dylib, RedwoodMetal) and the iOS files from
  `IOS_INPUT_DIR` (default ../sysimage). Missing from the prebuilts: the GPU layer (libOSMesa, libredwood-gl,
  libredwoodair) that `userland/build-gpu-y2s.sh` makes on macOS. Plan:
  1. A public CI job (ubuntu, like the private HakiOS-S20 x1s-mesa.yml) that runs `tools/mk-mesa.sh` with
     DRIVERS=softpipe,panfrost and AIR=1 and compiles libredwood-gl; artifact = our libraries only.
  2. On the owner's PC: unpack the IPSW as the earlier port did (E:\hakios-s20\tools\win-unpack-sysimage.sh, the
     backslash fix in select-system.py), compare Jesvi's sysimage/select-system.py with it, then
     `PREBUILT=1 GL=1 userland/build.sh` in Git Bash (as devices/x1s/build-images.sh did there: python3 shim,
     mkapfs links from manifests).
  3. Root from RAM over USB is not possible with this loader on Windows yet (no xl); first try a small root at boot
     +44 MiB (rpart=boot) and the system volume read from a partition, owner's OK before any super write.
- Root from RAM or a read-only partition first; no write of super/userdata without the owner's OK.
- The loader's 6 GB memory limit (MAX_MEM_SIZE) vs this phone's 12 GB.

## Steps

1. [running] Build Jesvi's own x1slte kernel on this fork's free public CI (baseline, unchanged source).
2. Add the x1s board profile: `common/devices/x1s.conf` (from x1slte.conf with the G981B memory map, storage
   layout, UEFI x1sPkg), `devices/exynos990/x1s/{device.json,target.conf}` with the measured values, and a kernel
   build `--device x1s` in CI.
3. Build Mu-Silicium x1sPkg (BootShim + UEFI) and Jesvi's loader into one boot image; pack the kernel and a
   small root as Jesvi's recovery image does, but for `boot`.
4. First boot on the phone (owner flashes boot in TWRP, then restarts through the menu): UEFI screen, loader, kernel
   log. Then the home screen.
5. Features one by one, each checked on the phone: display (60/120 Hz), touch, GPU and Metal, storage, Wi-Fi,
   battery/charging, sound, haptics, torch, sensors, cameras; cellular last (5G modem differs).

The earlier HakiOS-S20 port stays untouched as a backup (`E:\hakios-s20`, github steadyakh/hakios-s20).
