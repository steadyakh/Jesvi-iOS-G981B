# S20 auxiliary rear cameras: offline bring-up inventory

Branch `feat/s20-rear-aux`. Neither auxiliary camera is enabled or capture-tested.
The original front-camera path remains the baseline. Main rear quality remains
unresolved; this inventory is not a noise/tint fix.

## Reproduction

Run `python3 tools/camera/aux-inventory.py BASE.dtb OVERLAY0.dtb OVERLAY1.dtb`.
The checked-in JSON records SHA-256 identities and selected hardware properties
from the locally cached phone trees. No DTB, photo, calibration or firmware is
included. External overlay references are resolved through base `__symbols__`;
unresolved references stay symbolic. Cell arrays retain source order, including
power-sequence parameters. Numbered power-step names must be ordered numerically
when implementing sequencing; JSON object order is not execution order.

`python3 tools/camera/test_aux_inventory.py` covers external bus resolution,
missing-symbol preservation and invalid fixup offsets. Both cached overlays give
identical selected camera nodes; separate checks establish the addresses below.

## Wiring that must not be copied from the main/front camera

| Property | 2LA | GW2 |
|---|---|---|
| CIS seven-bit address | 0x2d | 0x2d |
| I2C controller | hsi2c_45, 0x15d70000 | hsi2c_1, 0x10560000 |
| Framework sensor instance | 4 | 2 |
| CSIS base | 0x17070000 | 0x17050000 |
| Logical MCLK | 3, gpc0-3 | 0, gpc0-0 |
| Reset | gpm33-0 | gpm32-0 |
| Declared active size | 4032×3024 | 9248×6936 |

GW2 module-node suffix `@5A` is not its CIS bus address. Linux sensor enums53/54
are not hardware model IDs. The base tree declares WDMA0 defaults for both, but the PHONE OVERLAY
changes GW2 to WDMA3 (0x170c4000, mux0x17020420). 2LA stays WDMA0
(0x170c1000, mux0x17020414). Initial bring-up remains exclusive. The GW2 controller recovery-GPIO metadata lists gpc0 pins, but its ACTIVE
pinctrl resolves to hsi2c1-bus, gpp0-2/3. Use the active pinctrl for native sensor
bus access; do not drive the clock pins as I2C based on recovery metadata.
Restore pin ownership on sensor changes and keep initial tests exclusive.

The source reference is ExtremeXT/android_kernel_samsung_exynos990 under
`drivers/media/platform/exynos/camera/sensor/module_framework/`: module_dt and
cis `is-cis-2la*` / `is-cis-gw2*`. Samsung's 2LA initialization branches on
revision A202: earlier revisions include Global_1, later revisions skip it;
initial_setting and Global_2 still apply. GW2 has separate TNP and Global tables.
Neither is a plain replacement of a front-camera setfile. Generic driver
aperture defaults do not establish this handset's optics. Physical lens roles,
FOV, model/revision and calibration must be verified before publishing metadata.

## Next hardware gates

1. Integrate latest accepted main/master into this branch before kernel/image
   builds. Do not flash an older candidate over another agent's accepted work.
2. Under exclusive phone ownership, implement bounded power/reset/MCLK control
   with rollback of shared rails and pins. Read model/revision before choosing
   revision-specific initialization. Do not invent expected IDs from enum values.
3. Add one sensor at a time with the actual controller/CSIS/WDMA route. Validate
   a bounded RAW capture, dimensions, packing, CFA and embedded/PDAF separation.
4. Validate calibrated preview and front/main/aux switching before exposing a
   Camera lens choice. Stock resolution/FPS tables are not verified iOS modes.

No auxiliary power writes, ID probes, streaming, kernel install or reboot were
performed for this checkpoint. Main-rear RAW format6 versus packed format7 and
native12MP readout remain separate pending experiments.

## 2026-11-15: bounded 2LA identity kernel prepared

Integrated `feat/s20-camera-quality` and accepted master8eaa52cf, including AOD
changes, in this auxiliary branch. No main/master pointers were moved.

Added explicit root debug command `cam.s20.2la.id` (no arguments). It reserves
idle camera ownership; refuses an open camera, active session, warm main sensor
or earlier failed cleanup. Device opens return EBUSY during the transaction.
The warm-main cooldown and sensor startup gaps are covered by the reservation.
Wait at least20seconds with Camera closed before a probe; busy is not a failed
sensor identity. It runs only when explicitly invoked, never during boot.

The probe sets reset low, applies the fixed rail transaction, takes split-bank
I2C pins, raises reset with the DT delay, enables CIS_CLK3 at26MHz on gpc0-3,
waits2ms and reads model0000/revision0002 at seven-bit address0x2d. It does not
scan addresses, write setfiles, start streaming, access CSI/PHY/DMA or change
front-camera tuning. Read identity is reported without assuming enum53 is an ID.
GPIO wire direction is based on the existing Samsung bus-pair convention;
a successful live identity read is still required to validate that assignment.

Power transaction (stock S2MPB02 formulas and phone DT):

| Rail | Register | Requested value |
|---|---|---|
| BUCK2 | 0x0d enable, 0x0e selector | Require existing selector0x90=1.3V; preserve lower control bits |
| LDO12 analog | 0x29 | selector0x58=2.8V, enable bit7 |
| LDO3 core | 0x20 | selector0x24=1.05V, enable bit7 |
| LDO8 IO | 0x25 | selector0x30=1.8V, enable bit7 |

Snapshot all control bytes before any writes. Reject any already-enabled dedicated
rail, or an unexpected shared buck voltage. The read-only live baseline showed
core selector0x20=1.0V, analog0x58 and IO0x30: merely toggling enable bits would
leave the core below the DT requirement. This is a bring-up finding for2LA,
not evidence explaining main-rear grain. Nothing was written to the live rails.

Cleanup holds reset low, stops/restores MCLK, restores I2C pin configuration,
restores all rail bytes in reverse order and restores reset/MCLK pin state.
Readback verifies writable clock fields and pin functions/pulls; input DAT and
clock status bits are not mistaken for persistent output latches. A PMIC NACK
may occur after a write latched, so the transaction marks the register before
writing it. Restore continues after individual errors. Cleanup failure poisons
auxiliary ownership and refuses later camera opens, rather than continuing with
unknown power state. Recovery then requires investigation and an approved reboot.

Validation: four inventory tests; ASan/UBSan fault injection of every rail
transaction operation (including latched-write NACKs, live-rail and shared-voltage
guards, cleanup error reporting); passcode build integration; full GICv2 kernel
build and incremental rebuild pass. Changed tracked and build-overlay files match.
These tests do not verify physical sensor identity or camera-open arbitration on
the device. The expanded inventory includes controller overlay properties and
supply voltages. Local pinctrl phandle numbers differ between saved overlays
(33/34 and36/37); these are references, not physical addresses. Previously
compared module/CIS wiring and supply voltages agree.

Private uninstalled candidate:
`<workspace>/build/camera-aux-20261115/`.
Stripped kernel15196056bytes, SHA256
`bc8e5120c206d404dba1b865a0fbe0e62197057960c62e60188de0d5493dfa54`.
Padded16MiB slot SHA256
`14f70dab3b2d63d0bf09e22b138f2d03c4cd7d9fd15609918262a5957da19700`.
Back up the CURRENT slot after acquiring exclusive ownership before deployment;
old rollback slots predate AOD. No flash or reboot yet. Explicit reboot approval
is required. GW2 still has no identity/stream implementation; its stock TNP and
shared OIS sequence must be handled separately after the first sensor is verified.

## Live result and next candidate (2026-11-15)

User approved ONE install/reboot. Current16MiB slot was backed up before any
writes: SHA256b686b63aae218f958e6212ee5bce95f44b2c5e92a4357a6bea0a8b858b17d266.
iOS raw-device writes were rejected read-only before mutation. Used loader Log
Access, staged only the padded kernel, `oem write recovery 2000000` reported
“written and read back”, then issued the single approved cold reboot. Loader,
root/system/data were not rewritten. Running kernel11:17:58IST, source8b39c494.
Protected AKS flags0x1 confirmed before first unlock and after identity probing.
User confirms front/main camera open and repeat lock/unlock work. Brightness
rule's foreground log appears after reboot; detailed slider policy not retested.

Two independent cold2LA ID transactions return model0x20ca/revision0xa202,
cleanup_ok=true. The complete read-only S2MPB02 map matches byte-for-byte across
the repeat. These establish sensor identity/power/I2C/MCLK, not image capture.
They select Samsung's A202 initialization without Global_1.

Next built candidate adds:

- `cam.s20.2la.stream`: exact model20ca/revisiona202 gate; stock initial, Global_2,
  native4032×3024@30 mode and single-sync tables; output-size readback; stream-on,
 12 polls over600ms checking frame counter0005 advances; stream-off and bounded
  stop-counter check before power cleanup. No CSIS/PHY/DMA writes and no RAW
  capture. Group hold stays unused as Samsung USE_GROUP_PARAM_HOLD=0. Register
  settings only: no Global_1, OTP or GW2 TNP firmware exported into git.
- `cam.s20.gw2.id`: active I2C1 gpp0-2/3 at0x2d, resetgpm32-0, MCLK0/gpc0-0;
  eight-rail snapshot across both camera PMICs, stock order/delays, fixed voltage
  selectors, shared live-rail voltage checks and reverse rollback. No setfiles or
  streaming. Reject incorrect existing parent BUCK1/BB/LDO18 setup without writes.

GW2 fixed rails in order: S2MPB03LDO6 analog2.8V, S2MPB03LDO1 core1.05V,
S2MPB02LDO13 AF2.8V, S2MPB03LDO7 OIS2.8V, S2MPB02LDO10 OIS1.8V,
S2MPB03LDO3 CAMIO1.8V, S2MPB02LDO6 VTIO1.8V, S2MPB02LDO7 SUBIO1.8V.
S2MPB03 bit6 is reserved and preserved. Shared rails restore exact prior bytes;
previously enabled dedicated rails prevent the probe. Both controller buses'
pin functions/pulls are read-back checked after restoration.

`catalogue-aux-setfiles.py` exports metadata/hashes only:9 2LA tables and14 GW2
tables, with direct page4000 output dimensions cross-checked for all6/12 modes.
The three catalogue tests include wrong-bank and mismatched-mode rejection.
GW2 two-PMIC fault injection covers every transaction operation, reserved bits,
parent/live/shared voltage guards, delay order and cleanup failures. A202 table
executor validates all input before writing, excludes burst firmware operations
and stops at every injected I2C failure. All sanitizer tests, inventory tests,
passcode integration and the new GICv2 kernel build pass. Sources match overlay.

Next candidate NOT installed. Private `kernel-slot-aux-next.bin`, SHA256
bdb51280424393a8abbee7f8a9e979c1811e0782d09095a58a031a9bb876a7b9,
stripped kernel15196112bytes, SHA256
 ae78c9ce6d683233cf60059d029ae7bf77c43f51362851fbfa4505ebae408f92.
Requires a NEW explicit install/reboot approval; the first approval is consumed.
After boot run ID commands, then2LA stream only if ID/cleanup pass. Close Camera,
wait20s, refuse busy rather than forcing ownership. Stop on failed cleanup; do
not run additional experiments against an unknown power state.

## Offline audit while phone is with the other session

User explicitly paused device work until they announce readiness. The second
reboot request is NOT approved. A took the phone for idle-power work; stop all
SSH/captures/changes. The previous in-flight slot read finished with a mismatch
against our installed candidate as the other session changed the kernel. It is
retained as `unverified-concurrent-slot-read.bin`, NOT a valid rollback image.
Do not assume our identity kernel remains installed. Integrate A's latest
accepted commits and freshly back up the actual slot after handback, before any
new deployment. Do not flash this branch's older candidate over their idle work.

The initial inventory missed receiver overlay fragments22/24. This is corrected:
extractor now retains explicit board `use_cphy`, DMA/VC channels, PHY references
and child routes. `aux-receiver-plan.py` requires those overrides and rejects
base-only/inconsistent routing. Four regression tests check this and both saved
overlays yield the same effective receiver plan:

| Property | 2LA | GW2 |
|---|---|---|
| Mode | C-PHY,3 trios | C-PHY,3 trios |
| CSIS |4:0x17070000 |2:0x17050000 |
| PHY analog block |0x170d2300 |0x170d1b00 |
| PHY reset bit index |2 |1 |
| WDMA |0 |3 |
| VC0 DMA |0x170c1000 |0x170c4000 |
| DMA common |0x170c1400 |0x170c4400 |
| Mux register |0x17020414 |0x17020420 |
| Mode0 speed label |1501 |2964 |
| Mode0 VC1 |unused |TAILPDAF,1152×1720 RAW10 |

Generic `phy-names=csis_dphy` does NOT select D-PHY. Board `use_cphy=1` is
explicit. Module common lane value2 is CSI_DATA_LANES_3 (three C-PHY trios), not
“two D-PHY lanes”. Source csi-v5_3 on CONFIG_PABLO_V8_0_0 selects PHY0503.0000,
mode0xC/type0xDC for CSI A/C/E, matching0/2/4. The7nm C-PHY settle table gives7
around the1501/1950/2964 source rates. Do not transplant the front four-lane
D-PHY setup. No receiver code is enabled in this checkpoint: first prove sensor
counter, then bounded CSI/VC0/DMA capture, excluding GW2 tail-PDAF from image data.

The GW2 journal now distinguishes a complete power-on from a partial failure.
Complete transactions restore rails in DT NORMAL_OFF order: AF, CAMIO, SUBIO,
VTIO, analog, core, OIS digital, OIS VM; AF-off delay9us. Partial failures roll
back touched registers in reverse order. Fault tests check exact successful
shutdown order/delay as well as every failed operation. This is still a bounded
diagnostic power path; production concurrency and optics are not implemented.

Additional MAIN-rear audit to measure after handback: source cam_s20_pmic03_bits
and redwood_s2mpb02_camera_bits change enable bits while preserving selectors.
DT requires S2MPB03 LDO2 core1.05V (selector0x1c), LDO5 analog2.95V (selector0x2e,
despite the rail's2.8V name), and S2MPB02 LDO1 sensor-PHY1.05V (selector0x24).
The read-only inactive S2MPB02 baseline had LDO1 selector0x18, which decodes to
0.9V; simply enabling that byte would retain0.9V. This is a source/snapshot
inference, NOT a live main-stream voltage measurement or a proven grain cause.
Check selectors while main is active, calibration/board requirements and fault
cleanup before proposing a rear-only trial. Do not change front tuning to test it.

Post-audit candidate (UNINSTALLED and must be rebased/rebuilt after A's handback):
slot SHA25638d0ab3da0312acde57bd45e71e5e2bc8962171e5aa5917d0e29592038e2cb83;
stripped kernel SHA2560723114933391c2e50d2c6a19b49a2dc93bc0d03934afd21fb691b96714a90f6.
Earlier bdb512... candidate is superseded and preserved privately as pre-audit.
