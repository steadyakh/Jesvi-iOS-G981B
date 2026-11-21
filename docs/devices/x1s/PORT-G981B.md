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
