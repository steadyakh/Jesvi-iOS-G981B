#!/bin/bash
# Build the user-space side of the bring-up.
#
#   hello            one raw-syscall program; as a RAM disk on its own it boots through mockfs
#   root.apfs        the small image: our init and test programs, etc/rc, and the launchd jobs
#                    (LaunchDaemons/), an APFS container (../tools/apfs/mkapfs.py). It travels in
#                    the payload next to the kernel (the second RAM disk, /dev/md1) and is mounted
#                    on /redwood. Rebuilt every time.
#                      RC=<file>    etc/rc: rc.launchd (default) hands process 1 to the iOS launchd
#                                   after init's checks; rc.ios runs a few iOS programs and halts
#                      JOBS="..."   labels of the stock iOS launchd jobs to load (default: IOS_JOBS)
#                      REST=1       ... and the rest of them, started only when asked for (see mkjobs.py)
#                      WIFID=0      without wifid, iOS's Wi-Fi daemon, which is in by default (see below)
#                      OSLOG="..."  labels of the jobs whose os_log output goes to the console ("all": every
#                                   one of JOBS; default: only the window server, which says little)
#                      INJECT="..." the programs started with a library of ours, "LIBRARY NAME..." per line
#                                   (default: src/inject.c in backboardd, src/inject_sb.c in SpringBoard,
#                                   src/inject_media.c in mediaserverd);
#                                   init hands the list to the kernel (etc/inject.conf -> sysctl kern.exec_inject)
#                      STOREFRONT=1 ... and src/inject_store.c inside appstored: the storefront answered,
#                                   and a bag request watched for (an experiment; see that file)
#                      EXTRA="..."  more programs for /redwood/bin (files on this machine): one-off
#                                   diagnostics, which a file given as RC_JOBS can then run
#                      EXTRA_SHARE="..." directories on this machine to copy whole into /redwood/share,
#                                   each under its own name: the data a one-off diagnostic reads, when it
#                                   needs more than a program (tools/air/intrincheck.py --emit writes such
#                                   a directory). This goes into the boot volume, which every recipe
#                                   carries and which the flash recipe puts in RAM, so its size is paid
#                                   for at every boot -- a few megabytes, not more
#                      RC_JOBS=<file> what the job com.redwood.demo runs: rc.jobs (default), or
#                                   rc.jobs.unlock, which also swipes the lock screen away (for the phone,
#                                   where nothing else can touch the screen yet).
#                                   A job list cannot assume a shell. iOS 16.5's own root has none at all
#                                   -- /bin is df and ps, and nothing else -- so a line beginning /bin/sh
#                                   fails with "No such file or directory" and status 127 on a flash-root
#                                   boot. The /var/jb/bin/sh that rc.jobs.jb and rc.jobs.sileo run is the
#                                   package manager's prefix, which only a JB=1 image carries. And init
#                                   runs these lines itself (src/init.c, run_command): words split on
#                                   blanks, no quoting, no variable assignment, no redirection beyond a
#                                   trailing ">> FILE" and "&". So a program that needs a value passed to
#                                   it needs an option for it, not an environment variable -- which is why
#                                   air_run grew --buffer.
#                      GL=1         with an OpenGL ES implementation for the window server: Mesa
#                                   (tools/mk-mesa.sh fetches and builds it) behind src/eagl.c, as
#                                   lib/libOSMesa.8.dylib (15 MB) and lib/libredwood-gl.dylib. Where the
#                                   kernel has started the phone's GPU, CoreAnimation then draws with its
#                                   OpenGL ES renderer, by that GPU (boot-arg gpu=1); anywhere else -- QEMU,
#                                   no boot-arg, no firmware in ../fw-gpu -- the library is not even loaded
#                                   and nothing changes (src/inject.c, opengl_es()).
#                                   GL=test: only the library and bin/gl_test (a triangle, src/gl_test.c;
#                                   RC_JOBS=rc.jobs.gltest runs it); the window server stays as it is.
#                      ADSP=1       with the audio DSP's firmware from ../fw-audio in fw/ (20 MB; for the
#                                   kernel's adsp_step=N, bsd/dev/redwood_adsp.c, which runs before the root is
#                                   mounted). Not beside GL=1: the two do not fit the 64 MB the loader takes with
#                                   the kernel. adsp=1 needs none of it: the system image has the firmware in /fw.
#   system.apfs      the system image, the root filesystem (the resident RAM disk, /dev/md0), an
#                    APFS container: the part of iOS 16.5 chosen by ../ios165/select-system.py at
#                    its real paths, the dyld shared cache, and src/stage0.c as /sbin/launchd (the
#                    real one is /sbin/launchd.ios). 2.5 GB; upload once per loader session with
#                    `xl ramdisk userland/system.apfs`. Rebuilt when missing, when its inputs
#                    (stage0, the manifest, the cryptex files) are newer, or with SYSTEM=1 (after
#                    changing the recipe below).
#   flashboot.apfs   FLASHBOOT=1: the RAM part of a boot whose root is the iOS volume on the phone's
#                    flash (see below); with CRYPTEXES=flash the boot volume alone, the cryptexes being
#                    on the flash. FLASHBOOT=1 also decides what goes into root.apfs: the job list is
#                    then made for the whole iOS volume (mkjobs.py --root-volume), so it holds daemons
#                    -- fairplayd, identityservicesd, passd ... -- that the system image has not got.
#                    That root.apfs is for a flash boot; a RAM boot wants one built without FLASHBOOT=1,
#                    or launchd will fail to start those few when something asks for them.
#
#   PREBUILT=1       do not compile anything: take our programs from ../prebuilt/userland (this is also
#                    what happens when the cross toolchain is not set up, see ../xnu-deps/setup.sh);
#                    the images are then made of them and of the iOS files alone (python3)
#   CHECK=1          check every image (every container) as it is made: apfsck has to find it clean and
#                    the kernel's apfs_lib has to read every volume back as the directory it was made of
#                    (../tools/apfs/check-image.sh; the system image takes a few minutes more)
set -e -o pipefail
cd "$(dirname "$(readlink -f "$0")")"
# On Windows (Git Bash, no WSL; the G981B port, docs/devices/x1s/PORT-G981B.md) the images are made with PREBUILT=1.
# Git Bash cannot make a symbolic link, so `ln -s` notes each one in obj/winlinks.tsv and ../tools/apfs/mkapfs.py
# (MKAPFS_LINKS) puts it into the image; the links of an unpacked tree come from its .manifest (note_links below).
# From the earlier HakiOS-S20 port's userland/build.sh, where it built that phone's images.
note_links() { :; }
case "$(uname)" in
MINGW*|MSYS*|CYGWIN*)
	mkdir -p obj
	export MKAPFS_LINKS=$(pwd -W)/obj/winlinks.tsv
	rm -f "$MKAPFS_LINKS"
	ln() {
		local s= f= a
		while [ "${1#-}" != "$1" ]; do
			case "$1" in *s*) s=1 ;; esac
			case "$1" in *f*) f=1 ;; esac
			shift
		done
		if [ -z "$s" ]; then command ln ${f:+-f} "$@"; return; fi
		[ -z "$f" ] || rm -f "$2"
		a="$(cd "$(dirname "$2")" && pwd -W)/$(basename "$2")"
		printf '%s\t%s\n' "$a" "$1" >> "$MKAPFS_LINKS"
	}
	# note_links SRC DST: the symbolic links listed in SRC.manifest (apfs-extract.py's), as links of DST
	note_links() {
		[ -f "$1.manifest" ] || return 0
		awk -F'\t' -v d="$(cd "$2" && pwd -W)" '$1 ~ /^12/ { print d "/" $4 "\t" $5 }' "$1.manifest" >> "$MKAPFS_LINKS"
	}
	;;
esac
source ../xnu/xnu-env.sh
# -no_fixup_chains: only Apple's own linker (macOS) knows it; the Linux iOS toolchain's ld (cctools-port) neither
# takes it nor makes chained fixups (the G981B port builds on a public Linux runner)
NO_FIXUP_CHAINS=
[ "$(uname)" = Darwin ] && NO_FIXUP_CHAINS=-Wl,-no_fixup_chains
source ./software-passcode-config.sh

CC="xnu-clang -arch arm64 -Os -static -nostdlib -ffreestanding -fno-stack-protector -fno-builtin -Wall -Wextra"
LD="-Wl,-e,_start -Wl,-pagezero_size,0x100000000"
DYN="xnu-clang -arch arm64 -isysroot ${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk -O1"
IOS=${IOS_INPUT_DIR:-../sysimage}
CACHE=$IOS/20F66__iPhone10,3_6
# ios165/rootfs and ios165/cryptex must be this copy's own directories of hard links, the way
# tools/new-workspace.sh makes them. A symlink to another copy's tree makes sysroot a symlink too, because
# cp -a does not follow one, and then every rm -rf and mv below edits the shared extraction in place. That
# happened on 19 October 2026: 491 LaunchDaemons, NanoLaunchDaemons, LaunchAngels, xpc/launchd.plist and
# private/var went out of the main copy's tree, and only the hard links in the other copies still held them.
for d in rootfs cryptex cryptex-app; do
	[ -L $IOS/$d ] || continue
	echo "build.sh: ios165/$d is a symlink to $(readlink $IOS/$d)" >&2
	echo "build.sh: use an independent input snapshot (tools/release/copy_tree.py)." >&2
	echo "build.sh: building against a directory link damages the shared tree." >&2
	exit 1
done
# The stock iOS jobs given to launchd, roughly in the order they were brought up.
IOS_JOBS=${JOBS-"
	com.apple.notifyd com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.logd
	com.apple.syslogd com.apple.distnoted.xpc.daemon com.apple.diagnosticd com.apple.fseventsd
	com.apple.containermanagerd com.apple.containermanagerd.system com.apple.runningboardd
	com.apple.backboardd com.apple.mobile.usermanagerd com.apple.mobilegestalt.xpc com.apple.lsd
	com.apple.powerd com.apple.analyticsd com.apple.logd_helper
	com.apple.iconservices.iconservicesagent com.apple.pluginkit.pkd
	com.apple.SpringBoard
"}
# iOS's own Wi-Fi daemon, for Settings to have a Wi-Fi to show: the kernel answers it in
# bsd/dev/redwood_apple80211.c. With the radio (wifi=1) Settings then shows Wi-Fi on, the network joined and the
# networks the radio hears. NOT FINISHED: a network cannot be joined from Settings yet.
# It is in by default now, because it costs nothing where there is no radio at all: in QEMU it comes up, talks
# to itself about country codes and sits there -- 114 processes started against 119 without it, no crash, never
# restarted -- and it answers what the rest of the system keeps asking for anyway. com.apple.wifi.manager, which
# wifid registers itself rather than declaring in its plist, fell from 30 failed lookups a session to 3,
# com.apple.private.corewifi-xpc from 1 to none, and the session's whole failed-lookup count from 331 to 305.
# (Count a QEMU run log with its duplicate lines removed: run-ios.sh prints launchd's log twice.)
# WIFID=0 leaves it out.
[ "${WIFID-1}" != 0 ] && IOS_JOBS="$IOS_JOBS com.apple.wifid"
# The S20+'s Bluetooth (bsd/dev/redwood_exynos_bt.c, bt=1): bluetoothd over the kernel's control
# com.apple.uart.bluetooth (H4), with BlueTool's stand-in (src/bluetool_stub.c); BT=0 leaves it out.
if [ "${BT-1}" != 0 ]; then
	IOS_JOBS="$IOS_JOBS com.apple.bluetoothd"
	export JOB_ENV="${JOB_ENV:+$JOB_ENV;}com.apple.bluetoothd:BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth"
	export ALLOW="${ALLOW:-} bluetooth"
fi
# The programs (file names) started with a library of ours inside them: "LIBRARY NAME..." per line
# (src/inject.c is libredwood.dylib, src/inject_sb.c is libredwood-sb.dylib, src/inject_media.c is
# libredwood-media.dylib).
# What Sileo runs from /bin (see the package manager's prefix below): the system image's /bin, or the
# boot volume's bin, bound over the iOS volume's /bin when that is the root (src/init.c, flash_root).
SILEO_BIN="chmod cp mkdir mv rm"
# (libredwood-tweaks is the tweak loader of the package manager's prefix, src/inject_tweaks.c: in
# SpringBoard the library above opens it, here it is named for the programs that have no library of
# ours -- Settings, where a tweak's own pane belongs. Add a program's name to have tweaks in it.)
INJECT=${INJECT-"/redwood/lib/libredwood.dylib backboardd containermanagerd containermanagerd_system locationd /Applications/ /private/var/containers/Bundle/Application/ /var/containers/Bundle/Application/
/redwood/lib/libredwood-sb.dylib SpringBoard
/redwood/lib/libredwood-touchid-ui.dylib SpringBoard
/redwood/lib/libredwood-media.dylib mediaserverd
/redwood/lib/libredwood-app.dylib Sileo
/redwood/lib/libredwood-tweaks.dylib Preferences imagent Camera
/redwood/lib/libredwood-files.dylib fileproviderd LocalStorageFileProvider SaveToFiles com.apple.DocumentManager.Service
/redwood/lib/libredwood-finder.dylib lockdownd
/redwood/lib/libredwood-eagl.dylib ${EAGL_APPS-AngryBirdsSpace Leos_fortune}
/var/jb/redwood/lib/libredwood-calls.dylib callservicesd
/var/jb/usr/lib/libredwood-metalon.dylib PhotosPosterProvider GradientPosterExtension EmojiPosterExtension
/var/jb/usr/lib/libredwood-duet.dylib duetexpertd
/redwood/lib/libredwood-mis.dylib misd sharingd
/redwood/lib/libredwood-mgcell.dylib Preferences"}
# (libredwood-metaloff is NOT in the boot list (B, 2026-11-14): it only matters while metal_props is on, and its
#  IORegistry hooks were the common factor of two stack overflows -- mediaserverd SIGBUS at its guard page, and
#  CollectionsPoster with metalon. Add it live to kern.exec_inject when metal_props is switched on.)
# libredwood-eagl (src/eagl_app.m): the OpenGL ES games named in EAGL_APPS (their executables' names) draw
# with Mesa instead of iOS's OpenGL ES on Metal, which hands Metal shaders only Apple's GPU compiler reads.
# STOREFRONT=1: and a library of ours inside appstored as well (src/inject_store.c). It is not in the
# list above because it changes what the daemon believes: AMS's account is made to answer with the
# storefront Apple's bag server returned for this device, which is how the App Store is asked whether
# a storefront is the only thing it is missing. It also writes down whether a bag request is ever
# built at all. For an experiment, not for an image anybody else boots.
[ -n "$STOREFRONT" ] && INJECT="$INJECT
/redwood/lib/libredwood-store.dylib appstored AppStore
/redwood/lib/libredwood-fairplay.dylib fairplayd.H2"
mkdir -p obj

if [ -z "$PREBUILT" ] && ! command -v ld64 >/dev/null; then PREBUILT=1; fi
if [ "${REQUIRE_SOURCE:-0}" = 1 ] && [ -n "$PREBUILT" ]; then
	echo "build.sh: release candidate requires source compilation; prebuilt fallback is disabled" >&2
	exit 1
fi
if [ -n "$PREBUILT" ]; then
	echo "build.sh: not compiling, using the programs in prebuilt/userland"
	for f in ../prebuilt/userland/*; do
		# keep a file that did not change (and its time stamp): system.apfs depends on stage0
		cmp -s "$f" "obj/$(basename "$f")" || cp "$f" obj/
	done
	cp obj/hello hello
else
	$CC $LD -o hello src/hello.c
	$CC -Ilibmini -c libmini/mini.c -o obj/mini.o
	$CC -c libmini/start.S -o obj/start.o
	for prog in init child stage0 var_store; do
		$CC -Ilibmini $LD -o obj/$prog.new src/$prog.c obj/mini.o obj/start.o
		# keep the old file (and its time stamp) when nothing changed: system.apfs depends on stage0
		cmp -s obj/$prog.new obj/$prog && rm obj/$prog.new || mv obj/$prog.new obj/$prog
	done
fi

# --- the small image ------------------------------------------------------------------------------
rm -rf root
mkdir -p root/sbin root/bin root/etc root/share root/LaunchDaemons
cp obj/init root/sbin/init
cp obj/child root/bin/child
# The data volume into the flash and back (src/var_store.c), and the job that saves it when a run
# ends: the program and its job together, away from the iOS job list below, which several working
# copies are editing at once.
[ -f obj/var_store ] && cp obj/var_store root/bin/
cp com.redwood.varstore.plist root/LaunchDaemons/
# Normal boot only mounts existing HFS data. The offline installer owns initialization;
# do not put a format kit beside init or make a missing filesystem authorize formatting.
echo "Darwin on redwood: image built $(date '+%Y-%m-%d %H:%M')" > root/etc/motd
# 300000 bytes, byte i = (i*31 + (i>>8)) & 0xff; init verifies it through read() and mmap()
python3 -c "import sys; sys.stdout.buffer.write(bytes((i*31 + (i>>8)) & 0xff for i in range(300000)))" > root/share/pattern.bin
# EXTRA_SHARE: whole directories of data for a one-off diagnostic, each under its own name. Here and
# not in root/bin, because what goes in is not a program: a generated test suite is modules and the
# bytes they read, and the job list runs it by its own run.sh. -L so that a directory assembled out
# of symbolic links arrives as files, since the image is made from this tree and not from the links'
# targets.
for d in $EXTRA_SHARE; do
	[ -d "$d" ] || { echo "build.sh: EXTRA_SHARE: $d is not a directory"; exit 1; }
	cp -rL "$d" root/share/
done
if [ -d $IOS/rootfs/usr/lib ]; then
	if [ -z "$PREBUILT" ]; then
		# ordinary dynamically linked programs of our own, built against the iOS SDK
		$DYN -o obj/hello_dyn src/hello_dyn.c
		$DYN -framework CoreFoundation -o obj/svc_test src/svc_test.c
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface -o obj/surf_test src/surf_test.c
		# an IOSurface of ours imported as a texture of the GPU, written through one view
		# and read through the other (src/drawable_test.c): the check that tells a texture
		# over the surface's own pages from a texture over a copy of them. It goes through
		# src/redwooddrawable.m, which is the bridge under test, and dlopens the compiler
		# library the way the driver does.
		$DYN -Wno-four-char-constants -framework CoreFoundation -framework IOSurface \
			-framework IOKit -o obj/drawable_test src/drawable_test.c src/redwooddrawable.m
		$DYN -o obj/rwtouch src/rwtouch.c
		# the kernel's thermal pressure passed on to iOS (src/thermald.c)
		$DYN -o obj/thermald src/thermald.c
		# native Touch ID: our fingerprint stack behind RedwoodMesa's bridge (src/redwood-fpd.c; runs only with touchid=1)
		$DYN -fblocks -Wno-four-char-constants -framework CoreFoundation -framework IOKit -o obj/redwood-fpd src/redwood-fpd.c
		$DYN -o obj/teetsd ../tools/fingerprint/teetsd.c
		$DYN -o obj/dsc-exec src/dsc_exec.c
		# Touch ID's place on screen: Apple's enrolment fingerprint moved onto the sensor, a ring while a capture waits
		$DYN -dynamiclib -fobjc-arc -fblocks $NO_FIXUP_CHAINS -framework UIKit -framework Foundation -framework CoreGraphics \
			-install_name /redwood/lib/libredwood-touchid-ui.dylib -o obj/libredwood-touchid-ui.dylib src/touchid_ui.m src/fp_ring.m
		# iOS's Low Power Mode switch passed to the kernel's clock governor (src/lpmd.c)
		$DYN -o obj/lpmd src/lpmd.c
		$DYN -framework CoreFoundation -framework IOKit -o obj/softkeybag-init src/software_keybag_init.c
		$DYN -framework Foundation -framework CoreMotion -o obj/redwood-motiond src/motion_wake.m
		{ ${LDID:-ldid} -Cadhoc -S obj/redwood-motiond 2>/dev/null || codesign -s - -f obj/redwood-motiond; }
		$DYN -framework CoreFoundation -framework AudioToolbox -framework AVFoundation -lobjc -o obj/sound_test src/sound_test.c
		$DYN -framework CoreFoundation -lMobileGestalt -o obj/mg_test src/mg_test.c
        $DYN -dynamiclib -fobjc-arc -fblocks $NO_FIXUP_CHAINS -framework UIKit -framework QuartzCore \
            -o obj/libredwood-graphics.dylib src/graphics_compat.m

		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/torch_test src/torch_test.c
		$DYN -O2 -o obj/camera_test src/camera_test.c src/camera_isp.c
		$DYN -fobjc-arc -framework Foundation -framework AVFoundation -o obj/camera_avtest src/camera_avtest.m
		# (the microphone as an application records: allowed to, by an entitlement, if ldid is there)
		$DYN -framework CoreFoundation -framework AVFoundation -lobjc -o obj/record_test src/record_test.c &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/record_test.entitlements obj/record_test 2>/dev/null ||
			  echo "build.sh: no ldid: record_test is unsigned, and the system may not let it record"; }
		$DYN -o obj/cpu_test src/cpu_test.c
		$DYN -framework CoreFoundation -framework IOSurface -o obj/cow_test src/cow_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework IOSurface -o obj/cg_test src/cg_test.c
		# what iOS here believes about Metal, and what CoreImage does without it (src/metal_test.m);
		# OpenGLES for the CIContext made over an EAGLContext, which is deprecated and the point
		$DYN -fobjc-arc -Wno-deprecated-declarations -framework Foundation -framework CoreGraphics \
			-framework CoreImage -framework IOKit -framework Metal -framework OpenGLES \
			-lMobileGestalt -o obj/metal_test src/metal_test.m
		# the first question of subsystem (2): can a subclass of the private _MTLDevice be
		# made here, and what does its initWithAcceleratorPort: do (src/metal_dev.m). No ARC:
		# it calls objc_msgSend by hand and allocates a class at runtime.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_dev src/metal_dev.m
		# a device object that implements nothing and logs what Apple's own init asks it
		# for (src/metal_stubdev.m); same reasons for no ARC and for -lobjc
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_stubdev src/metal_stubdev.m
		# the device object of the driver itself, as the library a bundle would hold
		# (src/redwoodmetal.m). It links against Metal's private _OBJC_CLASS_$__MTLDevice,
		# which is bindable because IOGPU.framework imports the same symbol; -lobjc for
		# class_addProtocol and no ARC because it calls objc_msgSend by hand.
		# (with src/redwooddrawable.m beside it: the IOSurface a CAMetalLayer hands a
		# device, as a texture. A file of its own because it answers CoreAnimation and
		# not Metal, and the two meet in one method -- docs/metal.md 10.46.)
		$DYN -dynamiclib -install_name /redwood/lib/libredwoodmetal.dylib \
			-framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/libredwoodmetal.dylib src/redwoodmetal.m src/redwooddrawable.m
		# and the program that walks Metal's own steps 4 and 5 against it
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/metal_drv_test src/metal_drv_test.m
		# An application of this port's own: a window, a CAMetalLayer, a drawable and a
		# triangle in it (src/triangle_app.m). UIKit because that is the point -- every
		# Metal result so far is from inside one of Apple's daemons, and this is an
		# ordinary bundle in /Applications that SpringBoard launches; IOSurface because
		# it reads the drawable's own surface back to say whether the texture WAS those
		# pages; no ARC and -lobjc because it sends -initWithAcceleratorPort: by hand,
		# the way src/metal_drv_test.m does. The bundle is assembled further down, where
		# a PREBUILT tree can assemble it too.
		# (signed, if ldid is there, for the entitlements an application has and a
		# /redwood/bin program does not: see src/triangle_app.entitlements, which says
		# why none of them is needed to RUN. Signed here rather than in the bundle, so
		# that the copy in /redwood/bin carries the same signature.)
		$DYN -framework Foundation -framework UIKit -framework QuartzCore \
			-framework Metal -framework IOSurface -framework IOKit -lobjc \
			-o obj/RedwoodTriangle src/triangle_app.m &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/triangle_app.entitlements obj/RedwoodTriangle 2>/dev/null ||
			  echo "build.sh: no ldid: RedwoodTriangle is unsigned, which still runs (AMFI here accepts everything) but holds no entitlements"; }
		# what the device ANSWERS when an application asks what it supports
		# (src/metal_caps.m). It takes the device from MTLCreateSystemDefaultDevice() so
		# that the ivars behind Apple's inherited answers are filled by the registration
		# Metal itself ran; no ARC and -lobjc because most of what it asks is MTLDeviceSPI,
		# whose header this port has not got, so every send is an objc_msgSend by hand. MetalFX
		# because the acceptance test's own capability gate is +supportsDevice: on its two
		# scaler descriptors, and that gate can be measured without the game.
		$DYN -framework Foundation -framework IOKit -framework Metal -lobjc \
			-framework MetalFX -o obj/metal_caps src/metal_caps.m
		# ... and the switch that steps 1 and 2 need: it puts MetalPluginName and
		# MetalPluginClassName on the IOAcceleratorES service (src/metal_props.c). IOKit and
		# CoreFoundation only -- a program that flips this switch must not itself ask Metal,
		# whose conditions are asserts. It needs the kernel booted with metal=1.
		$DYN -framework CoreFoundation -framework IOKit -o obj/metal_props src/metal_props.c
		$DYN -o obj/shm_test src/shm_test.c
		$DYN -o obj/apfs_mmap_test src/apfs_mmap_test.c
		$DYN -o obj/vol_ls src/vol_ls.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o obj/snap_test src/snap_test.c
		$DYN -framework CoreFoundation -framework CoreGraphics -framework CoreText -o obj/font_test src/font_test.c
		$DYN -fobjc-arc -framework Foundation -framework SystemConfiguration -o obj/net_test src/net_test.m
		# (the kernel's log over the network while iOS runs: PLANS/09-standalone.md, P4)
		$DYN -o obj/rwlogd src/rwlogd.c
		$DYN -o obj/m2m_mark src/m2m_mark.c
		$DYN -framework CoreFoundation -o obj/sd-userfs-start src/sd-userfs-start.c
		${LDID:-ldid} -Cadhoc -S obj/sd-userfs-start
		$DYN -framework CoreFoundation -o obj/prefset src/prefset.c
		$DYN -framework CoreFoundation -framework CoreLocation -lobjc -o obj/redwood-defaults src/redwood-defaults.c   # fresh-data-volume defaults, once
		$DYN -fobjc-arc -framework Foundation -o obj/safari_favourites src/safari_favourites.m
		${LDID:-ldid} -Cadhoc -S obj/safari_favourites
		$DYN -o obj/trollstore_once src/trollstore_once.c   # TrollStore on a fresh data volume, once
		${LDID:-ldid} -S../tools/redwood-defaults.entitlements obj/redwood-defaults
		${LDID:-ldid} -Cadhoc -S obj/prefset
		$DYN -framework CoreFoundation -o obj/redwood-cellulard ../tools/cellular/live-modem.c
		$DYN -o obj/cell-provision ../tools/cellular/cell-provision.c   # modem files from the phone's RADIO/EFS (read-only)
		$DYN -fblocks -o obj/bluetool_stub src/bluetool_stub.c
		# launchd panics on a non-platform binary for a system daemon, and in this port an ad hoc
		# signature is what makes one (bsd/kern/kern_minimac.c): sign the stub, or com.apple.BlueTool
		# takes the kernel down at ~30 s ("unexpected exec of non-platform binary").
		{ ${LDID:-ldid} -Cadhoc -S obj/bluetool_stub 2>/dev/null || codesign -s - -f obj/bluetool_stub 2>/dev/null; } ||
			echo "build.sh: bluetool_stub is unsigned: launchd will panic on com.apple.BlueTool"
		# (the account database answers only a client with the entitlements its own daemons hold: src/store_test.m)
		$DYN -fobjc-arc -framework Foundation -framework Accounts -o obj/store_test src/store_test.m &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/store_test.entitlements obj/store_test 2>/dev/null ||
			  echo "build.sh: no ldid: store_test is unsigned, and accountsd will not show it the accounts"; }
		# (with an entitlement for "battery_test open URL", if ldid is there: src/battery_test.c)
		$DYN -framework CoreFoundation -framework IOKit -lsqlite3 -o obj/battery_test src/battery_test.c &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/battery_test.entitlements obj/battery_test 2>/dev/null ||
			  echo "build.sh: no ldid: battery_test is unsigned, and SpringBoard will not open URLs for it"; }
		# (the keychain answers only a program that has a keychain access group: signed with one, if ldid is there)
		$DYN -framework CoreFoundation -framework Security -o obj/keychain_test src/keychain_test.c &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/keychain_test.entitlements obj/keychain_test 2>/dev/null ||
			  echo "build.sh: no ldid: keychain_test is unsigned, and the keychain will refuse it (-34018)"; }
		# (and wifid's way of keeping a network's password, by wifid's name: src/wifipw_test.c)
		$DYN -framework CoreFoundation -framework Security -o obj/wifipw_test src/wifipw_test.c &&
			{ ${LDID:-ldid} -Cadhoc -Ssrc/wifipw_test.entitlements obj/wifipw_test 2>/dev/null ||
			  echo "build.sh: no ldid: wifipw_test is unsigned, and the keychain will refuse it (-34018)"; }
		# our library for inside stock iOS programs (the jobs in INJECT are started with it)
		$DYN -dynamiclib -install_name /redwood/lib/libredwood.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood.dylib src/inject.c src/inject_sensors.c src/inject_location.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-sb.dylib -framework CoreFoundation -framework IOKit -lobjc \
			-o obj/libredwood-sb.dylib src/inject_sb.c src/inject_location.c
		# Persistent native AOD: a root-image copy, independent of existing /var/jb tweaks.
		$DYN -fobjc-arc -fblocks -DRW_ALWAYS_ON_ROOT_IMAGE -dynamiclib \
			-install_name /redwood/lib/libredwood-alwayson.dylib -framework Foundation -framework UIKit -framework QuartzCore \
			-F "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk/System/Library/PrivateFrameworks" -framework Preferences \
			-o obj/libredwood-alwayson.dylib ../tools/aod/alwayson.m ../tools/aod/settings.m
		$DYN -fobjc-arc -fblocks -framework Foundation -framework IOKit \
			-o obj/aod-preview ../tools/aod/control.m
		# ... the tweak loader of the package manager's prefix (src/inject_tweaks.c): it goes into a
		# program either as the one library the kernel inserts or opened by a library of ours
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-tweaks.dylib -framework CoreFoundation -lobjc \
			-o obj/libredwood-tweaks.dylib src/inject_tweaks.c
		# Personal Hotspot: misd's carrier answers (src/mis_ct.m) and Settings' "cellular-data" (src/mg_cellular.c)
		$DYN -fobjc-arc -fblocks -dynamiclib -install_name /redwood/lib/libredwood-mis.dylib -framework Foundation -lobjc \
			-F "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk/System/Library/PrivateFrameworks" -framework CoreTelephony \
			-o obj/libredwood-mis.dylib src/mis_ct.m
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-mgcell.dylib -framework CoreFoundation -lobjc \
			-o obj/libredwood-mgcell.dylib src/mg_cellular.c
		# ... and the network behind the access point (tools/hotspot: addresses, NAT, CLAT46; misctl for tests)
		mkdir -p obj/hotspot-inc/net obj/hotspot-inc/libkern
		# XNU's headers: the developer tree, or (a release checkout has none) the release kernel's prepared tree
		xs=../xnu-src/xnu-8796.121.2
		[ -f $xs/bsd/net/pfvar.h ] || xs=$(ls -td ../.cache/release-kernel/*/ 2>/dev/null | while read -r d; do [ -f "$d/bsd/net/pfvar.h" ] && { echo "${d%/}"; break; }; done)
		[ -n "$xs" ] && [ -f "$xs/bsd/net/pfvar.h" ] || { echo "hotspot: no XNU headers (run ./build.sh kernel first)"; exit 1; }
		cp "$xs/bsd/net/pfvar.h" "$xs/bsd/net/radix.h" obj/hotspot-inc/net/
		cp "$xs/libkern/libkern/tree.h" obj/hotspot-inc/libkern/
		for p in rwdhcpd rwpf clat-test; do $DYN -Iobj/hotspot-inc -o obj/$p ../tools/hotspot/$p.c; done
		# rwsys NAME [VALUE] (sysctl strings): the Store account boot job uses it (it was copied below but never built)
		$DYN -o obj/rwsys src/rwsys.c
		$DYN -fobjc-arc -framework Foundation -F "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk/System/Library/PrivateFrameworks" \
			-framework MobileWiFi -o obj/misctl ../tools/hotspot/misctl.m &&
			{ ${LDID:-ldid} -S../tools/hotspot/misctl.ent obj/misctl 2>/dev/null || echo "build.sh: no ldid: misctl unsigned"; }
		# Files on the HFS+ data volume: target only its daemon and local provider.
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-files.dylib -lobjc -lsandbox \
			-o obj/libredwood-files.dylib src/inject_files.c
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-finder.dylib \
			-F "${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk/System/Library/PrivateFrameworks" -framework CoreFoundation -framework MobileActivation \
			-o obj/libredwood-finder.dylib src/inject_finder.c
		$DYN -o obj/LocalFilesHost src/local_files_host.c
		$DYN -fobjc-arc -framework Foundation -framework Security -o obj/register_finder_host src/register_finder_host.m
		${LDID:-ldid} -S../tools/storage/register-finder-host.entitlements obj/register_finder_host
		$DYN -framework CoreFoundation -framework SystemConfiguration -o obj/device_name src/device_name.c
		# ... and inside an application, to say what killed it (src/inject_app.c)
		$DYN -dynamiclib -install_name /redwood/lib/libredwood-app.dylib -lobjc \
			-o obj/libredwood-app.dylib src/inject_app.c
		# (+ src/h264_encoder.c: a software H.264 encoder for VideoToolbox, the S20 having no AVE; OpenH264 from
		#  ../../third-party/openh264, built with: make OS=ios ARCH=arm64 SDK_MIN=16.0 SDKROOT=<sdk> libraries)
		OH=${OPENH264:-../../third-party/openh264}
		if [ "${DEVICE:-x1slte}" = x1slte ]; then
			CAMERA_PERMANENT=1 CAMERA_SDK="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" CAMERA_OPENH264="$OH" sh ../tools/camera/build-profile-media.sh obj/camera-permanent
			cp obj/camera-permanent/libredwood-media-dev.dylib obj/libredwood-media.dylib
			CAMERA_SDK="${THEOS:-$HOME/theos}/sdks/iPhoneOS16.5.sdk" ../tools/camera/build-image-hooks.sh obj
		else
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-media.dylib -framework CoreFoundation -framework CoreMedia \
			-framework CoreVideo -framework IOSurface -framework VideoToolbox -lobjc -lc++ -I$OH/codec/api \
			-o obj/libredwood-media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c src/h264_encoder.c \
			$OH/libopenh264.a
		fi
		# ... and inside appstored (STOREFRONT=1), which answers with a storefront and watches for a bag
		# request being built (src/inject_store.c)
		# (-lc++abi for __cxa_throw: every C++ exception thrown in the process is watched, which is how
		#  the jetpack parser's rejections are seen -- a throw somebody catches logs nothing of itself)
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-store.dylib -framework CoreFoundation -lobjc \
			-lc++abi -o obj/libredwood-store.dylib src/inject_store.c
		# ... and inside fairplayd.H2, which starts and leaves at once: what it looks for, and where it
		# gives up (src/inject_fairplay.c; watches only, replaces nothing)
		$DYN -O2 -dynamiclib -install_name /redwood/lib/libredwood-fairplay.dylib -framework CoreFoundation \
			-framework IOKit -framework Security -lMobileGestalt \
			-o obj/libredwood-fairplay.dylib src/inject_fairplay.c
		# the output device of the audio server: a plug-in for CoreAudio's HAL
		$DYN -bundle -framework CoreFoundation -o obj/RedwoodAudio src/audio_plugin.c
		# The same driver again as a loadable bundle, which is the form Metal's own
		# discovery wants: step 3 builds the path /System/Library/Extensions/<name>.bundle
		# and opens it with NSBundle, and -[NSBundle classNamed:] then tests that the class
		# came out of that bundle (0x1811e0dac). The dylib above cannot serve as the
		# executable: dyld uniques images by install_name, so a process that already has
		# /redwood/lib/libredwoodmetal.dylib would bind the bundle to it and fail that test.
		$DYN -bundle -framework Foundation -framework IOKit -framework Metal -lobjc \
			-o obj/RedwoodMetal src/redwoodmetal.m src/redwooddrawable.m
		if [ -n "$GL" ]; then
			# the OpenGL ES library, and the table of functions that hands CoreAnimation's calls to it
			# (Mesa says beside the library what it was built from: one from other patches is built again)
			M=../xnu-deps/build/mesa/out
			# (again with the drivers it was built with: softpipe,panfrost for the Exynos phone's Mali)
			[ "$(cat $M/built-from 2>/dev/null)" = "$(cat ../tools/mesa/*.patch ../tools/mesa/redwood-drm/* | sha256sum)" ] ||
				DRIVERS=$(cat $M/drivers 2>/dev/null) ../tools/mk-mesa.sh
			# ... and the compiler library separately, because the hash above covers the PATCHES
			# and the library is built from tools/mesa/air/, which they do not mention. Two boots
			# of 22 October carried a libredwoodair.dylib three hours older than the driver beside
			# it while rwa_render_shape had grown IN THE MIDDLE -- the image was the mismatch the
			# contract warns about, and nothing in it said so. Silence is what made it expensive:
			# the library is stripped, so the driver found the symbols it wanted and read a stride
			# where a pixel format was.
			[ "$(cat $M/built-from-air 2>/dev/null)" = "$(cat ../tools/mesa/air/* ../tools/mesa/redwood-drm/* ../tools/mesa/*.patch | sha256sum)" ] || {
				echo "build.sh: libredwoodair.dylib is older than what it is built from (mk-mesa.sh's hash)"
				AIR=1 DRIVERS=$(cat $M/drivers 2>/dev/null) ../tools/mk-mesa.sh
			}
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl.c > obj/eagl_gen.h
			$DYN -dynamiclib -install_name /redwood/lib/libredwood-gl.dylib -Iobj -framework CoreFoundation \
				-framework IOKit -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-gl.dylib \
				src/eagl.c src/gl_osmesa.c
			# OpenGL ES for games by Mesa instead of iOS's GL-on-Metal (src/eagl_app.m); no -framework
			# OpenGLES: its gl* names must bind to Mesa's, or every call loops back into iOS's library
			./mkeagl.py src/eagl_slots.txt $M/gl.xml $M/exported-gl.txt src/eagl_app.m > obj/eagl_app_gen.h
			$DYN -fobjc-arc -dynamiclib -install_name /redwood/lib/libredwood-eagl.dylib -Iobj -framework Foundation \
				-framework QuartzCore -framework IOSurface -lobjc $M/libOSMesa.8.dylib -o obj/libredwood-eagl.dylib \
				src/eagl_app.m
			$DYN -framework CoreFoundation -framework IOKit $M/libOSMesa.8.dylib -o obj/gl_test src/gl_test.c
			# ... and what this GPU makes of an unsigned integer turned into a float (src/u2f_test.c)
			$DYN $M/libOSMesa.8.dylib -o obj/u2f_test src/u2f_test.c
			cp $M/libOSMesa.8.dylib obj/
			# air_run draws with a Metal shader translated by the AIR frontend
			# and reads the pixels back (docs/dev/air.md). It is built inside the
			# Mesa build, because it needs Gallium itself and not the gl*
			# entry points this library exports; AIR=1 tools/mk-mesa.sh makes
			# it, and it is copied in when it is there.
			[ -f $M/air_run ] && cp $M/air_run obj/air_run
			# The compiler the Metal driver asks for: AIR in, a dispatch on the
			# Adreno out (tools/mesa/air/air_metal.h). It is copied whether or not
			# GL is wanted, because src/redwoodmetal.m dlopens it by that path --
			# and its absence is a sentence the driver prints, not a crash.
			[ -f $M/libredwoodair.dylib ] && cp $M/libredwoodair.dylib obj/
		fi
	fi
	# (drawable_test is built in the same block as surf_test, so it is copied with it)
	# (a prebuilt/ from before a program has none: copy what is there)
	for p in hello_dyn svc_test surf_test drawable_test rwtouch mg_test; do
		if [ -f obj/$p ]; then cp obj/$p root/bin/; fi
	done
	[ -f obj/cpu_test ] && cp obj/cpu_test root/bin/        # (a prebuilt/ from before this program has none)
	[ -f obj/thermald ] && cp obj/thermald root/bin/
	[ -f obj/redwood-fpd ] && cp obj/redwood-fpd obj/teetsd obj/dsc-exec root/bin/
	# the S20 modem service (com.redwood.cellular), built in the x1slte-cellular branch (tools/cellular/live-modem.c)
	if [ -n "$PREBUILT" ]; then
		[ ! -f ../prebuilt/userland/redwood-cellulard ] || cp ../prebuilt/userland/redwood-cellulard root/bin/
	else
		cp obj/redwood-cellulard root/bin/
		[ -f obj/cell-provision ] && cp obj/cell-provision root/bin/
	fi
	# The phone's call and text pieces that run from the data volume (B, 2026-11-15): put in place by init on a data
	# volume that has none (src/init.c seed_phone; a newer one put there by hand is kept). Without this a fresh or wiped
	# phone booted with the jobs (com.redwood.telephonyd, callservicesd) but not their programs: no calls, no texts.
	# callservicesd (5 MB, too big for this image) comes from the system image (/System/Library/Redwood/Phone).
	mkdir -p root/seed; : > root/etc/seed.list
	for x in redwood-ims:755:/private/var/root/redwood-ims \
	         redwood-telephonyd:755:/private/var/jb/redwood/bin/redwood-telephonyd \
	         libredwood-calls.dylib:755:/private/var/jb/redwood/lib/libredwood-calls.dylib \
	         RedwoodSMS.dylib:755:/private/var/jb/usr/lib/TweakInject/RedwoodSMS.dylib \
	         RedwoodCellular.dylib:644:/private/var/jb/usr/lib/TweakInject/RedwoodCellular.dylib; do
		f=${x%%:*}; rest=${x#*:}; mode=${rest%%:*}; dst=${rest#*:}
		if [ -f ../prebuilt/userland/$f ]; then cp ../prebuilt/userland/$f root/seed/$f; printf '%s\t0\t0\t%s\t/seed/%s\n' $mode $dst $f >> root/etc/seed.list
		else echo "build.sh: prebuilt/userland/$f missing: a fresh phone will have no calls/texts piece $dst" >&2; fi
	done
	for x in RedwoodSMS RedwoodCellular; do
		[ -f ../tools/cellular/$x.plist ] && cp ../tools/cellular/$x.plist root/seed/$x.plist &&
			printf '644\t0\t0\t/private/var/jb/usr/lib/TweakInject/%s.plist\t/seed/%s.plist\n' $x $x >> root/etc/seed.list
	done
	printf '755\t0\t0\t/private/var/jb/redwood/callservicesd\t/System/Library/Redwood/Phone/callservicesd\n' >> root/etc/seed.list
	[ -f obj/lpmd ] && cp obj/lpmd root/bin/
	[ -f obj/softkeybag-init ] && cp obj/softkeybag-init root/bin/
	[ -f obj/redwood-motiond ] && cp obj/redwood-motiond root/bin/
	[ -f obj/cow_test ] && cp obj/cow_test root/bin/
	[ -f obj/cg_test ] && cp obj/cg_test root/bin/
	[ -f obj/metal_dev ] && cp obj/metal_dev root/bin/
	[ -f obj/metal_stubdev ] && cp obj/metal_stubdev root/bin/
	[ -f obj/metal_drv_test ] && cp obj/metal_drv_test root/bin/
	[ -f obj/metal_caps ] && cp obj/metal_caps root/bin/
	[ -f obj/metal_props ] && cp obj/metal_props root/bin/
	[ -f obj/redwood-defaults ] && cp obj/redwood-defaults root/bin/
	[ -f obj/safari_favourites ] && cp obj/safari_favourites root/bin/
	[ -f obj/trollstore_once ] && cp obj/trollstore_once root/bin/
	[ -f obj/sd-userfs-start ] && cp obj/sd-userfs-start root/bin/
	[ -f obj/prefset ] && cp obj/prefset root/bin/          # (x1slte: preferences without Settings' UI)
	[ -f obj/shm_test ] && cp obj/shm_test root/bin/
	[ -f obj/apfs_mmap_test ] && cp obj/apfs_mmap_test root/bin/
	[ -f obj/vol_ls ] && cp obj/vol_ls root/bin/
	[ -f obj/snap_test ] && cp obj/snap_test root/bin/
	[ -f obj/font_test ] && cp obj/font_test root/bin/
	[ -f obj/metal_test ] && cp obj/metal_test root/bin/
	[ -f obj/net_test ] && cp obj/net_test root/bin/
	[ -f obj/rwlogd ] && cp obj/rwlogd root/bin/
	[ -f obj/m2m_mark ] && cp obj/m2m_mark root/bin/
	[ -f obj/rwsys ] && cp obj/rwsys root/bin/          # (sysctl strings from the Mac: the sound debug port)
	[ -f obj/bluetool_stub ] && cp obj/bluetool_stub root/bin/
	[ -f obj/store_test ] && cp obj/store_test root/bin/
	[ -f obj/battery_test ] && cp obj/battery_test root/bin/
	[ -f obj/keychain_test ] && cp obj/keychain_test root/bin/
	[ -f obj/wifipw_test ] && cp obj/wifipw_test root/bin/
	[ -f obj/sound_test ] && cp obj/sound_test root/bin/
	[ -f obj/torch_test ] && cp obj/torch_test root/bin/
	[ -f obj/camera_test ] && cp obj/camera_test root/bin/
	[ -f obj/camera_avtest ] && cp obj/camera_avtest root/bin/
	[ -f obj/record_test ] && cp obj/record_test root/bin/
	# The same binary as the application below, under /redwood/bin, for its --layer mode:
	# a CAMetalLayer with no window and no UIKit, driven once. That mode cannot say whether
	# a frame reaches the panel, and it answers the two questions that come before it -- does
	# the driver work outside Apple's daemons, and does nextDrawable give a texture -- in a
	# process whose output is certainly in the log, because the job list runs it. An
	# application launched by SpringBoard is a process this file cannot promise that about.
	[ -f obj/RedwoodTriangle ] && cp obj/RedwoodTriangle root/bin/triangle_app
	for f in $EXTRA; do cp "$f" root/bin/; done
	#
	# ... and the application bundle itself, staged in obj/ and copied into
	# sysroot/Applications by the system image below.
	#
	# A PLAIN DIRECTORY OF REAL FILES, and that is not a style: a bundle made of symbolic
	# links is refused by LaunchServices ("Unable to register"), and the registrar that
	# walks /Applications at boot passes over mount points. So the icons are generated here
	# rather than linked from anywhere, and the plist and the executable are copied.
	if [ -f obj/RedwoodTriangle ]; then
		rm -rf obj/RedwoodTriangle.app
		mkdir -p obj/RedwoodTriangle.app
		cp obj/RedwoodTriangle obj/RedwoodTriangle.app/
		cp src/triangle_app.plist obj/RedwoodTriangle.app/Info.plist
		printf 'APPL????' > obj/RedwoodTriangle.app/PkgInfo
		# The icon is the picture the application draws: an orange triangle over the
		# lower left half of a blue square, by the same rule the shader covers a pixel
		# by ((x+0.5)/w < (y+0.5)/h). So the home screen states the prediction, and an
		# icon that comes out with its halves the other way round says the arithmetic in
		# this one line disagrees with the arithmetic in src/triangle_app.m. No alpha
		# channel: iOS masks an icon itself and an icon with alpha comes out wrong.
		python3 - obj/RedwoodTriangle.app <<-'PY'
		import struct, sys, zlib
		out = sys.argv[1]
		for size, scale in ((120, 2), (180, 3)):
		    rows = bytearray()
		    for y in range(size):
		        rows.append(0)                        # PNG filter type: none
		        for x in range(size):
		            inside = (2 * x + 1) * size < (2 * y + 1) * size
		            rows += bytes((255, 128, 0) if inside else (0, 0, 255))
		    def chunk(kind, body):
		        return (struct.pack('>I', len(body)) + kind + body +
		                struct.pack('>I', zlib.crc32(kind + body)))
		    png = (b'\x89PNG\r\n\x1a\n' +
		           chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 2, 0, 0, 0)) +
		           chunk(b'IDAT', zlib.compress(bytes(rows), 9)) + chunk(b'IEND', b''))
		    open('%s/AppIcon60x60@%dx.png' % (out, scale), 'wb').write(png)
		PY
		# Unsigned, which is enough: AMFI in this kernel accepts every signature and
		# holds no trust cache (libkern/amfi/miniamfi.c), and the kernel is built without
		# config_enforce_signed_code. What a signature would still buy is entitlements,
		# and this application asks the system for nothing that needs one.
		echo "  RedwoodTriangle.app: $(du -sk obj/RedwoodTriangle.app | cut -f1) KB for /Applications"
	fi
	# /dev and /private/var exist so that this image can also be the root on its own:
	# devfs is mounted on /dev, and init puts a ramfs on /private/var
	mkdir -p root/dev root/private/var
	mkdir -p root/lib
	cp obj/libredwood.dylib obj/libredwood-sb.dylib root/lib/
	[ ! -f obj/libredwood-touchid-ui.dylib ] || cp obj/libredwood-touchid-ui.dylib root/lib/
	[ -f obj/libredwood-media.dylib ] && cp obj/libredwood-media.dylib root/lib/     # (as with cpu_test)
	[ -f obj/libredwood-store.dylib ] && cp obj/libredwood-store.dylib root/lib/
	[ -f obj/libredwood-fairplay.dylib ] && cp obj/libredwood-fairplay.dylib root/lib/
	[ -f obj/libredwood-app.dylib ] && cp obj/libredwood-app.dylib root/lib/
	[ -f obj/libredwood-tweaks.dylib ] && cp obj/libredwood-tweaks.dylib root/lib/
	[ -f obj/libredwood-files.dylib ] && cp obj/libredwood-files.dylib root/lib/
	[ -f obj/libredwood-finder.dylib ] && cp obj/libredwood-finder.dylib root/lib/
	[ -f obj/libredwood-mis.dylib ] && cp obj/libredwood-mis.dylib root/lib/
	[ -f obj/libredwood-mgcell.dylib ] && cp obj/libredwood-mgcell.dylib root/lib/
	for p in rwdhcpd rwpf clat-test misctl; do [ ! -f obj/$p ] || cp obj/$p root/bin/; done
	cp ../tools/hotspot/hotspot-up.sh root/bin/ && chmod 755 root/bin/hotspot-up.sh
	[ -f obj/register_finder_host ] && cp obj/register_finder_host root/bin/
	[ -f obj/LocalFilesHost ] && cp obj/LocalFilesHost root/bin/
	[ -f obj/device_name ] && cp obj/device_name root/bin/
	[ -f obj/libredwoodmetal.dylib ] && cp obj/libredwoodmetal.dylib root/lib/
	[ -f obj/libredwoodair.dylib ] && cp obj/libredwoodair.dylib root/lib/
	if [ -f obj/RedwoodAudio ]; then
		mkdir -p root/lib/RedwoodAudio.driver
		cp obj/RedwoodAudio root/lib/RedwoodAudio.driver/
		cp src/audio_plugin.plist root/lib/RedwoodAudio.driver/Info.plist
	fi
	# The Metal plug-in bundle, flat and unsigned -- which is enough here: AMFI in this
	# kernel is a stand-in that accepts every signature (libkern/amfi/miniamfi.c), and
	# Apple's own code already loads the unsigned RedwoodAudio.driver above. The system image
	# now links this bundle into /System/Library/Extensions as well (see below), but the two
	# properties that would make Metal look for it are still nobody's by default: only a boot
	# with metal=1 lets a process put them on the service (docs/metal.md 10.31), because 10.14
	# measured what a half-made device does to every process that asks Metal for one.
	if [ -f obj/RedwoodMetal ]; then
		mkdir -p root/lib/RedwoodMetal.bundle
		cp obj/RedwoodMetal root/lib/RedwoodMetal.bundle/
		cp src/metal_plugin.plist root/lib/RedwoodMetal.bundle/Info.plist
	fi
	if [ -n "$GL" ]; then
		cp obj/libOSMesa.8.dylib root/lib/
		cp obj/gl_test root/bin/
		if [ -f obj/u2f_test ]; then cp obj/u2f_test root/bin/; fi   # an "if": see the inject.conf loop below
		[ -f obj/air_run ] && cp obj/air_run root/bin/
		[ "$GL" = test ] || cp obj/libredwood-gl.dylib root/lib/
		[ -f obj/libredwood-eagl.dylib ] && cp obj/libredwood-eagl.dylib root/lib/
	fi
	# a program is not started with a library that is not there: it would not start at all
	echo "$INJECT" | while read -r library names; do
		# a /redwood/lib library only if it was actually built; any other path (e.g. a tweak loader
		# in /var/jb) as it is, since it lives on the prefix and is there at run time
		case "$library" in
		"") ;;
		# an "if" and not "&& echo": a last line whose library is absent would make the loop,
		# and with it the pipeline and the whole build under set -e, fail -- which is what a tree
		# with only prebuilt/userland in it does, since not every injection library is shipped there
		/redwood/lib/*) if [ -f "root/lib/${library#/redwood/lib/}" ]; then echo "$library $names"; fi ;;
		*) echo "$library $names" ;;
		esac
	done > root/etc/inject.conf
	if [ "${DEVICE:-x1slte}" = x1slte ]; then
		python3 ../tools/aod/prepare-root.py root --binaries obj
	fi
	cp $IOS/var.skel root/etc/var.skel              # the directories init creates in /private/var
	# time zones: the template's links go in a circle until the time zone daemon has run (/usr/share/zoneinfo
	# points back into /var); point them at the data that ships with the system, and use this machine's zone
	zone=$(timedatectl show -p Timezone --value 2>/dev/null || true)
	# macOS: /etc/localtime -> /var/db/timezone/zoneinfo/Area/City
	[ -n "$zone" ] || zone=$(readlink /etc/localtime 2>/dev/null | sed -n 's|.*/zoneinfo/||p')
	zone=${zone:-UTC}
	# perl, not sed -i: BSD sed takes the next argument as -i's suffix and does not know \t
	ZONE=$zone perl -pi -e 's|^l\t/usr/share/zoneinfo\t/private/var/db/timezone/zoneinfo$|l\t/usr/share/zoneinfo.default\t/private/var/db/timezone/zoneinfo|;
		s|^l\t/var/db/timezone/zoneinfo/[^\t]*\t/private/var/db/timezone/localtime$|l\t/var/db/timezone/zoneinfo/$ENV{ZONE}\t/private/var/db/timezone/localtime|' root/etc/var.skel
	# ... and the files of ours it puts there (var/): those under mobile/ belong to the user mobile
	printf 'd\t755\t0\t0\t/private/var/preferences/SystemConfiguration\n' >> root/etc/var.skel
	cp -r var root/var
	(cd var && find . -type f | sed 's|^\./||' | sort) | while read -r f; do
		case $f in mobile/*) owner=501 ;; *) owner=0 ;; esac
		printf 'f\t644\t%s\t%s\t/private/var/%s\t/var/%s\n' $owner $owner "$f" "$f"
	done >> root/etc/var.skel
	# mobile's own temporary directory, $HOME/tmp, which iOS's template does not have: an application of
	# mobile's without a data container of its own keeps its temporary files there -- Sileo, registered from
	# the prefix with the root on the flash, downloads every package and index there, and without it each
	# download ended in "No such file or directory"
	printf 'd\t700\t501\t501\t/private/var/mobile/tmp\n' >> root/etc/var.skel
	# A key of ours for root, when there is one: ssh/authorized_keys, never in the repository (like
	# fw-*). dropbear from the package manager's prefix reads it and lets us in over the phone's
	# Wi-Fi, which saves rebuilding an image for every question one wants to ask a running iOS. It
	# refuses a key whose directory anyone but its owner may write, hence the 700.
	# SHARE=1: no keys in an image that is going to somebody else. ssh_host_ed25519_key is a
	# private key, and it would be leaving this machine inside a 2.5 GB container
	if [ -z "$SHARE" ] && [ -f ../ssh/authorized_keys ]; then
		mkdir -p root/var/root/.ssh
		printf 'd\t700\t0\t0\t/private/var/root/.ssh\n' >> root/etc/var.skel
		# the key that lets us in, and the host's own key beside it: made here and not on the
		# phone, because ssh-keygen asks who uid 0 is and gets "No user exists for uid 0" --
		# it asks through libiosexec, which reads a database the bootstrap does not ship (see
		# init.c, where it is now built at every boot); the key is made here all the same,
		# since nothing should depend on the order of those two
		for k in authorized_keys ssh_host_ed25519_key ssh_host_ed25519_key.pub; do
			[ -f ../ssh/$k ] || continue
			cp ../ssh/$k root/var/root/.ssh/$k
			printf 'f\t600\t0\t0\t/private/var/root/.ssh/%s\t/var/root/.ssh/%s\n' $k $k >> root/etc/var.skel
		done
	fi
	cp ${RC:-rc.launchd} root/etc/rc
	cp ${RC_JOBS:-rc.jobs} root/etc/rc.jobs
	cp m2m.tour root/etc/m2m.tour        # mali2metal's standard tour (rwlogd GET /tour)
	# QUIET=1: launchd's own standard error goes nowhere (src/init.c). It says a few thousand
	# lines about its own business in the first minutes and pushes everything else out of the
	# kernel's log; the daemons keep their own standard error on the console all the same.
	rm -f root/etc/quiet
	# on by default since 2026-11-14 (B): launchd's notices were the largest share of the log; QUIET=0 brings them back
	[ "${QUIET-1}" != 0 ] && : > root/etc/quiet
	# the jobs the iOS launchd will see (/System/Library/LaunchDaemons is a symlink to this directory).
	# With FLASHBOOT=1 the root will be Apple's whole volume on the flash, not the part of it that fits in
	# the system image, so mkjobs.py judges "is this daemon's program there" by the listing of the volume
	# and not by what select-system.py extracted here (--root-volume; seven jobs turn on the difference,
	# fairplayd among them).
	cp com.redwood.demo.plist com.redwood.wifi.plist com.redwood.thermald.plist com.redwood.lpmd.plist com.redwood.logd.plist com.redwood.cellular.plist root/LaunchDaemons/
	[ ! -x root/bin/safari_favourites ] || cp com.redwood.safari-favourites.plist root/LaunchDaemons/
	[ ! -x root/bin/redwood-defaults ] || cp com.redwood.defaults.plist root/LaunchDaemons/   # fresh-volume defaults
	[ ! -x root/bin/trollstore_once ] || cp com.redwood.trollstore.plist root/LaunchDaemons/   # TrollStore, once
	cp com.redwood.account-store.plist root/LaunchDaemons/     # (the Store relay at every boot, if installed)
	[ ! -x root/bin/redwood-motiond ] || cp com.redwood.motion-wake.plist root/LaunchDaemons/
	[ ! -x root/bin/redwood-fpd ] || cp com.redwood.fpd.plist root/LaunchDaemons/
    # Shared graphics is automatic for normal applications. GRAPHICS_COMPAT=0
    # retains the previous discovery behavior for diagnosis.
    if [ "${GRAPHICS_COMPAT-1}" != 0 ] && [ -n "$GL" ] && [ "$GL" != test ]; then
        python3 ../tools/graphics/prepare-root.py root --binaries obj
    fi
	# calls on iOS's own call screen (A, 2026-11-14): callservicesd (CallKit's daemon; the system image leaves framework
	# executables out) runs from /var/jb/redwood (5 MB: too big for root.apfs in RECOVERY; tools/cellular/RedwoodPhone/
	# install.sh puts it there from the iOS rootfs), its own launchd job pointed there, and RedwoodPhone (installed in
	# /var/jb/Applications) as the CallKit provider of redwood-ims
	# Calls (A, 2026-11-15; native only, the user's decision): redwood-telephonyd (/var/jb/redwood/bin, installed by
	# tools/cellular/telephonyd/install.sh) is the telephony CallKit provider under CommCenter's identity. RedwoodPhone,
	# the third-party style provider of the old engine, is no longer shipped.
	CALL_PROVIDER=../tools/cellular/telephonyd/com.redwood.telephonyd.plist
	CSD=System/Library/PrivateFrameworks/TelephonyUtilities.framework/callservicesd
	if [ "${REQUIRE_SOURCE:-0}" = 1 ]; then
		[ -f "$IOS/rootfs/$CSD" ] || { echo "build.sh: callservicesd missing from prepared extraction" >&2; exit 1; }
	else
		[ -f $IOS/rootfs/$CSD ] || { [ -f $IOS/rootfs.dmg ] && 7z x -y -o$IOS/rootfs $IOS/rootfs.dmg $CSD >/dev/null 2>&1; } || true
	fi
	if [ -f $IOS/rootfs/$CSD ] && [ -f $IOS/rootfs/System/Library/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist ]; then
		# The plist is rewritten with plistlib after mkjobs below, which handles both binary and XML.
		cp "$IOS/rootfs/System/Library/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist" root/LaunchDaemons/
		cp $CALL_PROVIDER root/LaunchDaemons/
	else
		# a machine without the extracted iOS root (B, 2026-11-14: a macOS build left mkjobs' stock job, which
		# points at /System/.../callservicesd -- absent -- so launchd failed it with EX_CONFIG and no incoming
		# call reached the call screen): the job as it runs on the phone, kept in the tree
		cp com.apple.telephonyutilities.callservicesd.plist root/LaunchDaemons/
		cp $CALL_PROVIDER root/LaunchDaemons/
	fi
	# Native USB mux services: loopback only, socket activated, no panic-on-crash policy.
	cp com.redwood.usbmux-lockdown.plist root/LaunchDaemons/
	[ ! -x root/bin/register_finder_host ] || cp com.redwood.finder-host.plist root/LaunchDaemons/
	[ -x ../jb/root/var/jb/usr/sbin/sshd ] && [ -f root/var/root/.ssh/authorized_keys ] && cp com.redwood.sshd.plist root/LaunchDaemons/
	[ "${BT-1}" != 0 ] && cp com.apple.BlueTool.plist root/LaunchDaemons/
	# native Touch ID: the iPhone 8's biometrickitd job (Touch ID module only); it starts when RedwoodMesa publishes
	# AppleBiometricServices, i.e. only with touchid=1 (redwood-fpd). Needs libBKDM1 in the system image.
	[ ! -x root/bin/redwood-fpd ] || cp com.apple.biometrickitd.mesa.plist root/LaunchDaemons/
	VOLUME=""
	[ -n "$FLASHBOOT" ] && [ -f $IOS/rootfs.slt ] && VOLUME="--root-volume $IOS/rootfs.slt"
	# (A, 2026-11-14: no job's os_log on the console by default -- with OS_ACTIVITY_DT_MODE backboardd, the window server,
	# wrote every touch's log lines out on its own threads; stream them with /var/jb/usr/bin/oslog instead, or build
	# with OSLOG=com.apple.backboardd for the console copy)
	[ -n "$IOS_JOBS" ] && SD_USERFS=${DEVICE:-x1slte} OSLOG=${OSLOG-} ./mkjobs.py ${REST:+--rest} $VOLUME $IOS/rootfs root/LaunchDaemons $IOS_JOBS
	# after mkjobs, which writes the stock callservicesd job over ours (B, 2026-11-14): the program is in /var/jb/redwood
	# (python, not plutil: on macOS the build's PATH has xnu-deps/host-macos/bin/plutil, a no-op stand-in)
	if [ -f root/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist ]; then
		python3 -c 'import plistlib,sys
p=sys.argv[1]; d=plistlib.load(open(p,"rb")); d["ProgramArguments"]=["/var/jb/redwood/callservicesd"]; d.pop("Program",None)
plistlib.dump(d,open(p,"wb"))' root/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist
	fi
fi
# Explicit experimental image profile; ordinary builds retain their current job list.
# Match with softpasscode=1 in the kernel boot line. Restore the saved software bag
# before launchd, and use the native daemon with private file permissions.
if [ "${SOFTWARE_PASSCODE:-0}" = 1 ]; then
	python3 ../tools/passcode/prepare-root.py root --binaries obj
fi
# firmware of the phone's own hardware, where somebody has pulled it from the phone's Android into ../fw-gpu
# (it is the vendor's and never in the repository): the kernel's GPU bring-up, bsd/dev/redwood_adreno.c, finds it in fw/
if [ -d ../fw-gpu ]; then
	mkdir -p root/fw
	cp ../fw-gpu/* root/fw/
fi
# ... and the image of the Wi-Fi DSP, from /vendor/firmware_mnt/image of the same Android into ../fw-wifi
# (bsd/dev/redwood_wifi.c; 7.6 MB), with what the radio's firmware is sent once it runs: this phone's
# board data and the regulatory database (image/qca6750 there; 60 and 24 KB)
if [ -f ../fw-wifi/wpss.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-wifi/wpss.mdt ../fw-wifi/wpss.b0[2-9] root/fw/
	for f in bd_m20gl.elf regdb_xiaomi.bin; do
		[ -f ../fw-wifi/qca6750/$f ] && install -m 0644 ../fw-wifi/qca6750/$f root/fw/
	done
	# the network the radio is to join: its name on the first line, its passphrase on the second. Yours, and
	# so not in the repository; it goes where the firmware goes and the kernel reads it there (wifi_join=1)
	rm -f root/fw/network
	# SHARE=1: leave it out, for an image that is going to somebody else -- it is a passphrase
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# the VTS (voice block) firmware, the phone's /vendor/firmware/vts.bin, into ../fw-sound (Samsung's: never committed):
# the S20's microphones go through VTS, and its Cortex-M core runs during capture (bsd/dev/redwood_exynos_sound.c mic.vts)
if [ -f ../fw-sound/vts.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-sound/vts.bin root/fw/
fi
# ... or the S20+'s Broadcom BCM4375 (bsd/dev/redwood_bcm4375.c, wifi_pcie=4), from the phone's /vendor/firmware
# into ../fw-wifi under these names: bcmdhd_sta.bin_b1, bcmdhd_clm.blob and the B1 NVRAMs (SEMCO's is this
# phone's, "Y2"; Murata's for wifi_nvram=1). 1.3 MB, the vendor's and not in the repository.
if [ -f ../fw-wifi/bcm4375-sta.bin ]; then
	mkdir -p root/fw
	# (and bcm4375B1-semco.hcd: the Bluetooth side's patchram, the phone's /vendor/firmware/bcm4375B1_semco.hcd,
	# bsd/dev/redwood_exynos_bt.c)
	for f in bcm4375-sta.bin bcm4375-clm.blob bcm4375-nvram-semco.txt bcm4375-nvram-murata.txt bcm4375B1-semco.hcd bcm4375B1-murata.hcd; do
		[ -f ../fw-wifi/$f ] && install -m 0644 ../fw-wifi/$f root/fw/
	done
	# the network the kernel joins at the start (name, then passphrase; in no repository), as above
	rm -f root/fw/network
	if [ -z "$SHARE" ] && [ -f ../fw-wifi/network ]; then install -m 0600 ../fw-wifi/network root/fw/network; fi
fi
# ... and the vibration motor's waveforms, the phone's /vendor/firmware/aw8697_haptic.bin, from ../fw-haptic
# (bsd/dev/redwood_haptic.c, haptic=3; 3.6 KB, the vendor's and not in the repository either)
if [ -f ../fw-haptic/aw8697_haptic.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-haptic/aw8697_haptic.bin root/fw/
fi
# x1slte: the sensor hub's firmware (bsd/dev/redwood_ssp.c, shub=4 and up), made by
# tools/sensors/mk-fw-sensors.py out of Samsung's picasso patch into ../fw-sensors (Samsung's/Broadcom's: never committed)
if [ -f ../fw-sensors/bcm_sensorhub_a1.bin ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-sensors/bcm_sensorhub_a1.bin root/fw/
fi
# x1slte: the Galaxy S20's CS40L25 needs Cirrus's DSP program and wavetable (bsd/dev/redwood_cs40l25.c, haptic_chip=cs40l25)
for f in cs40l25a.wmfw cs40l20.bin; do
	if [ -f ../fw-haptic/$f ]; then
		mkdir -p root/fw
		install -m 0644 ../fw-haptic/$f root/fw/
	fi
done
# ... and the cameras' register tables (bsd/dev/redwood_camera.c, cam=2 and up), made by tools/camsensor.py out of
# the sensors' module files of Android's camera stack, the phone's /vendor/lib64/camera/com.qti.sensormodule.
# redwood_<maker>_<sensor>_<camera>.bin, in ../fw-camera (the vendor's: neither they nor the tables are in the
# repository): camera-<sensor>.tab
for module in ../fw-camera/com.qti.sensormodule.redwood_*.bin; do
	[ -f "$module" ] || continue
	sensor=$(basename "$module" .bin | sed 's/^com\.qti\.sensormodule\.redwood_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camsensor.py "$module" "root/fw/camera-$sensor.tab" >/dev/null
done
# ... and their colour tuning (libredwood-media's processor, src/camera_isp.c), made by tools/camtuning.py out of the
# cameras' tuning files beside them, com.qti.tuned.redwood_<maker>_<sensor>_<camera>.bin: camera-<sensor>.isp
for tuned in ../fw-camera/com.qti.tuned.redwood_*.bin; do
	[ -f "$tuned" ] || continue
	sensor=$(basename "$tuned" .bin | sed 's/^com\.qti\.tuned\.redwood_[a-z]*_\([a-z0-9]*\)_.*/\1/')
	mkdir -p root/fw
	../tools/camtuning.py "$tuned" "root/fw/camera-$sensor.isp" >/dev/null
done
# ... and with ADSP=1 the image of the audio DSP, from the same place into ../fw-audio (bsd/dev/redwood_adsp.c):
# adsp.mdt and adsp.b02 to adsp.b36, 20 MB. Only when asked for: beside GL=1 the payload would be more than the 64 MB
# the loader takes.
if [ -n "$ADSP" ] && [ -f ../fw-audio/adsp.mdt ]; then
	mkdir -p root/fw
	install -m 0644 ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] root/fw/
fi
# Firmware that is not there is otherwise silent at build time and speaks only much later, deep in a
# device log -- "adreno: THE GPU IS NOT STARTED: the small image has no fw/a660_gmu.bin" cost a phone
# slot on 19 October 2026. It is the vendor's and never in the repository, so a copy made with bare
# git worktree add has none of it, and neither has one whose tools/new-workspace.sh predates the fw-*
# loop there. Say what went in and what was not there to go in; what each one is for is above.
fwhave= fwnone=
for d in gpu wifi haptic camera audio; do
	if [ -d ../fw-$d ]; then fwhave="$fwhave $d"; else fwnone="$fwnone $d"; fi
done
echo "build.sh: fw/: $(ls -1 root/fw 2>/dev/null | wc -l) files from ../fw-*:${fwhave:- none present}${fwnone:+, absent:$fwnone}"
# The images are what somebody would be handed to run this on a second phone, and two of the things
# build.sh puts in them are secrets: the Wi-Fi network's name and passphrase, and the host's own
# private key for ssh. Both are one small file inside 2.5 GB, and nobody would notice either leave.
secrets=
[ -f root/fw/network ] && secrets="$secrets fw/network (the name and passphrase of your Wi-Fi)"
[ -f root/var/root/.ssh/ssh_host_ed25519_key ] &&
	secrets="$secrets /var/root/.ssh/ssh_host_ed25519_key (a private key)"
if [ -n "$secrets" ]; then
	echo "build.sh: this image carries:$secrets"
	echo "build.sh: do not pass it on as it is; SHARE=1 builds the same image without them."
fi
# When the iOS volume on the phone's flash is to become the root (boot-arg root_apfs=ufs_darwin,
# with flashboot.apfs resident; src/init.c, flash_root): an empty directory to hide iOS's
# NanoLaunchDaemons and LaunchAngels with,
# xpc without launchd.plist (launchd would read that list of all of iOS's jobs instead of
# LaunchDaemons) and the audio server's plug-in directory with only our device in it.
mkdir -p root/empty root/xpc root/hal
for f in extensions.plist extensions.plist.sig; do
	[ -f $IOS/rootfs/System/Library/xpc/$f ] && cp $IOS/rootfs/System/Library/xpc/$f root/xpc/
done
[ -d root/lib/RedwoodAudio.driver ] && ln -s /redwood/lib/RedwoodAudio.driver root/hal/RedwoodAudio.driver
# launchd panics on a non-platform binary for a system daemon, and in this port an ad hoc signature
# is what makes one (bsd/kern/kern_minimac.c). Sign the BlueTool stand-in here, where every build
# path passes (macOS copies the program from prebuilt/userland and never compiles it).
if [ -f root/bin/bluetool_stub ]; then
	{ ${LDID:-ldid} -Cadhoc -S root/bin/bluetool_stub 2>/dev/null ||
	  codesign -s - -f root/bin/bluetool_stub 2>/dev/null; } ||
		echo "build.sh: bluetool_stub is unsigned: launchd will panic on com.apple.BlueTool"
fi
# the same for dsc-exec, which starts biometrickitd under com.apple.biometrickitd.mesa (Touch ID): unsigned, launchd
# panics ("unexpected exec of non-platform binary: /redwood/bin/dsc-exec", on the S20 2026-11-18)
if [ -f root/bin/dsc-exec ]; then
	{ ${LDID:-ldid} -Cadhoc -S root/bin/dsc-exec 2>/dev/null || codesign -s - -f root/bin/dsc-exec 2>/dev/null; } ||
		{ echo "build.sh: dsc-exec is unsigned: launchd would panic on com.apple.biometrickitd.mesa; leaving its job out" >&2
		  rm -f root/LaunchDaemons/com.apple.biometrickitd.mesa.plist; }
fi
# the image: zlib where it saves room (a small file whole in its attribute, as Apple keeps them); fw/
# as it is, which the kernel's drivers use where it lies in memory (bsd/dev/redwood_image.c)
# callservicesd runs from /var/jb/redwood, whatever wrote its job (mkjobs, or a stale one when the job section was
# skipped on a machine without the extracted iOS root): checked here, right before the boot volume is made (B, 2026-11-14)
if [ -f root/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist ]; then
	python3 -c 'import plistlib,sys
p=sys.argv[1]; d=plistlib.load(open(p,"rb")); d["ProgramArguments"]=["/var/jb/redwood/callservicesd"]; d.pop("Program",None)
plistlib.dump(d,open(p,"wb"))' root/LaunchDaemons/com.apple.telephonyutilities.callservicesd.plist
fi
# per-device mods (devices/exynos990/.../root/ and customize.sh), only for release builds that name a target
if [ -n "${JESVI_TARGET:-}" ]; then
	python3 ../tools/release/apply_mods.py "$JESVI_TARGET" root root
fi
if [ "${DEVICE:-x1slte}" = x1slte ] && [ -d "$IOS/rootfs/usr/lib" ]; then
    python3 ../tools/camera/prepare-root.py root --binaries obj
fi
python3 ../tools/apfs/mkapfs.py -z --plain fw -o root.apfs redwood=root | tail -1
[ -z "$CHECK" ] || ../tools/apfs/check-image.sh root.apfs redwood=root

# --- the RAM part of a boot off the flash: FLASHBOOT=1 -> flashboot.apfs --------------------------------
# With the iOS volume on the phone's flash as the root, all that has to come from RAM is what is not
# on it -- the two cryptexes, OS with the dyld shared cache and App -- and what starts the boot: three
# APFS containers back to back in the one resident image. The first, the boot volume, is the root the
# kernel starts from (rd=md0): stage0 as /sbin/launchd mounts the small image on /redwood and runs its
# init, which with boot-arg root_apfs=DEV mounts the volume on /flash, makes it the root and moves the
# boot volume to the volume's /private/preboot, then mounts the second container -- the OS cryptex -- on
# the boot volume's empty Cryptexes/OS and the third -- Apple's App cryptex as it is -- on Cryptexes/App:
# /private/preboot/Cryptexes/OS and App, where the volume's own /System/Cryptexes/OS and App point and
# where iOS grafts the cryptexes (src/init.c, flash_root; the kernel's apfs takes "/dev/md0,c=1" and
# "c=2", and bsd/vm/vm_unix.c takes a shared cache from Cryptexes/OS). CRYPTEX_IMAGE=FILE:
# Apple's own image of the cryptex as it is instead of this one remade (2.6 GB against 1 GB: too much
# for the phone's memory beside iOS, not for QEMU's). Upload it with `xl ramdisk userland/flashboot.apfs`.
# CRYPTEXES=flash: the boot volume alone (20 MB) -- the cryptexes are Apple's images as they are on the
# flash, in darwin right after the iOS volume (../tools/darwin-write.py put them there), and init mounts
# them from there, the same containers 1 and 2 of the root's own device.
if [ -n "$FLASHBOOT" ] && [ -f $CACHE/dyld_shared_cache_arm64 ]; then
	rm -rf flashboot flashboot.os
	mkdir -p flashboot/sbin flashboot/redwood flashboot/dev flashboot/flash flashboot/Cryptexes/OS flashboot/Cryptexes/App
	cp obj/stage0 flashboot/sbin/launchd
	# the OS cryptex, and the shared cache in it, only where this image will carry it:
	# with CRYPTEXES=flash the cryptexes are read from the phone's flash and this tree is
	# built and thrown away unused. The guard ends with the cache: everything after it goes
	# into the boot volume, which every recipe carries, flash cryptexes included
	if [ "$CRYPTEXES" != flash ]; then
		if [ -d $IOS/cryptex ]; then python3 ../tools/release/copy_tree.py "$IOS/cryptex" flashboot.os; note_links "$IOS/cryptex" flashboot.os; else mkdir -p flashboot.os; fi
		D=flashboot.os/System/Library/Caches/com.apple.dyld
		mkdir -p $D
		for f in $CACHE/dyld_shared_cache_arm64*; do
			case "$f" in *.symbols) continue ;; esac
			# -f: a cryptex fetched with the cache already in it would otherwise stop the build
			ln -f "$f" $D/
		done
	fi
	# the audio DSP's firmware and the speaker amplifiers' configuration, as in the system image's /fw: the kernel
	# reads the firmware as soon as there is a root (this image, adsp=1), the configuration maybe later -- by then
	# from the iOS volume's /fw, which src/init.c (flash_root) binds to this
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] flashboot/fw/
	fi
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p flashboot/fw
		cp ../fw-audio/aw882xx_acf.bin flashboot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: /jb of the boot volume, which is
	# /private/preboot/jb once the iOS volume is the root -- the volume where a rootless jailbreak keeps its
	# Procursus on an iPhone as well; init grafts it onto /var/jb (src/init.c, mount_writable)
	if [ -d ../jb/root/var/jb ]; then
		cp -a ../jb/root/var/jb flashboot/jb
		mkdir -p flashboot/jb-rw        # where init mounts the prefix's writable copy (src/init.c)
		# and what Sileo runs from /bin, for init to put over the iOS volume's /bin (see the system image below)
		mkdir -p flashboot/jb-bin
		for c in $SILEO_BIN; do
			ln -s /var/jb/usr/bin/$c flashboot/jb-bin/$c
		done
		echo "  the package manager's prefix: $(du -sh flashboot/jb | cut -f1) as /jb of the boot volume"
	fi
	python3 ../tools/apfs/mkapfs.py -z --plain fw -o flashboot.boot.apfs Boot=flashboot | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.boot.apfs Boot=flashboot
	# the third container: Apple's App cryptex as it is (10 MB; fetch.sh), for the boot volume's Cryptexes/App
	APP=""
	[ -f $IOS/cryptex-app.dmg ] && APP=$IOS/cryptex-app.dmg
	if [ "$CRYPTEXES" = flash ]; then
		mv flashboot.boot.apfs flashboot.apfs
	elif [ -n "$CRYPTEX_IMAGE" ]; then
		cat flashboot.boot.apfs "$CRYPTEX_IMAGE" $APP > flashboot.apfs
	else
		python3 ../tools/apfs/mkapfs.py -z -o flashboot.os.apfs OS=flashboot.os | tail -1
		[ -z "$CHECK" ] || ../tools/apfs/check-image.sh flashboot.os.apfs OS=flashboot.os
		cat flashboot.boot.apfs flashboot.os.apfs $APP > flashboot.apfs
		rm -f flashboot.os.apfs
	fi
	rm -rf flashboot flashboot.os flashboot.boot.apfs
	echo "flashboot.apfs: $(( $(stat -c %s flashboot.apfs) / 1048576 )) MB"
fi

# --- the system image -----------------------------------------------------------------------------
SYSTEM_CACHE_HIT=0
if [ -n "${HACKIOS_IMAGE_CACHE:-}" ]; then
	cache_rc=0
	python3 ../tools/release/system_cache.py restore --ios "$IOS" ${SYSTEM:+--force} || cache_rc=$?
	case "$cache_rc" in
		0) SYSTEM_CACHE_HIT=1 ;;
		10) SYSTEM=1 ;; # An input changed: do not trust the legacy mtime shortcut.
		*) exit "$cache_rc" ;;
	esac
fi
if [ "$SYSTEM_CACHE_HIT" = 0 ]; then
if [ -f $IOS/system.manifest ] && [ -f $CACHE/dyld_shared_cache_arm64 ] &&
	{ [ ! -f system.apfs ] || [ obj/stage0 -nt system.apfs ] || [ obj/LocalFilesHost -nt system.apfs ] ||
	  [ ../tools/storage/stage-provider.py -nt system.apfs ] || [ $IOS/system.manifest -nt system.apfs ] ||
	  [ $IOS/cryptex -nt system.apfs ] || [ $IOS/cryptex-app -nt system.apfs ] || [ -n "$SYSTEM" ]; }; then
	# what ios165/select-system.py has chosen since ios165/rootfs was extracted is not in it, and would not be
	# in the image either (so it was once with the audio plug-ins and the system's sounds: iOS found no
	# speaker and played nothing, though our device was there and the kernel's side worked)
	if [ "${REQUIRE_SOURCE:-0}" = 1 ]; then
		python3 ../tools/release/extraction.py verify
	elif [ -f $IOS/.done-rootfs ] && [ $IOS/select-system.py -nt $IOS/.done-rootfs ]; then
		echo "build.sh: ios165/select-system.py has changed since ios165/rootfs was extracted: run KEEP=1 ios165/fetch.sh" >&2
		exit 1
	fi
	rm -rf sysroot
	python3 ../tools/release/copy_tree.py "$IOS/rootfs" sysroot
	# launchd must load our choice of jobs, not all 500 that iOS has
	rm -rf sysroot/System/Library/LaunchDaemons sysroot/System/Library/NanoLaunchDaemons \
		sysroot/System/Library/LaunchAngels sysroot/System/Library/xpc/launchd.plist
	ln -s /redwood/LaunchDaemons sysroot/System/Library/LaunchDaemons
	mkdir -p sysroot/redwood sysroot/sbin
	# Touch ID's sensor module: the iPhone 8's libBKDM1, ported onto this (iPhone X) cache by tools/touchid/relink_bkdm1.py
	# (tools/touchid/fetch-bkdm1.sh; Apple's code, never in the repository). biometrickitd loads it from /usr/lib: every
	# install has Touch ID without the iPhone 8's 3 GB cache (A, 2026-11-20). Sparse: ~130 MB, mostly zeros.
	if [ -f ../sysimage/iphone8/libBKDM1.dylib ]; then
		cp ../sysimage/iphone8/libBKDM1.dylib sysroot/usr/lib/libBKDM1.dylib
		chmod 755 sysroot/usr/lib/libBKDM1.dylib
		echo "  Touch ID: ported libBKDM1.dylib in /usr/lib"
	fi
	# The default wallpaper (B, 2026-11-14, the user's pick): a Photos poster with its lock-screen clock style,
	# layout and paired home screen -- PosterBoard's store (PRBPosterExtensionDataStore) reduced to that one poster.
	# init copies it into a data volume that has no poster store yet (userland/src/init.c, default_wallpaper); a
	# phone with wallpapers of its own is never touched. 3.5 MB (the editing source is HEIC; snapshots are left out,
	# PosterBoard renders them again).
	if [ -d defaults/wallpaper/PRBPosterExtensionDataStore ]; then
		mkdir -p sysroot/System/Library/Redwood/DefaultWallpaper
		cp -R defaults/wallpaper/PRBPosterExtensionDataStore sysroot/System/Library/Redwood/DefaultWallpaper/
		find sysroot/System/Library/Redwood/DefaultWallpaper -type d -exec chmod 0777 {} +
		find sysroot/System/Library/Redwood/DefaultWallpaper -type f -exec chmod 0644 {} +
	fi
	# The fingerprint TA set for a clean data volume (A, 2026-11-20): redwood-fpd copies it to /var/root/tee when that
	# is missing (src/redwood-fpd.c fp_provision). It is Samsung's vendor/tee from THIS phone (device/firmware/vendor-tee),
	# Private release builds take it from ../fw-fingerprint (a private input like fw-sound/fw-wifi, the same vendor/tee
	# folder); a build for this machine's own phone also from device/firmware/vendor-tee (docs/clean-install.md).
	FPTEE=
	if [ -d ../fw-fingerprint ]; then FPTEE=../fw-fingerprint
	elif [ -z "$SHARE" ] && [ -d ../../../device/firmware/vendor-tee ]; then FPTEE=../../../device/firmware/vendor-tee; fi
	if [ -n "$FPTEE" ]; then
		mkdir -p sysroot/System/Library/Redwood/Fingerprint
		rm -rf sysroot/System/Library/Redwood/Fingerprint/tee
		cp -R "$FPTEE" sysroot/System/Library/Redwood/Fingerprint/tee
		find sysroot/System/Library/Redwood/Fingerprint/tee -name '._*' -delete
		chmod -R go-rwx sysroot/System/Library/Redwood/Fingerprint/tee
		echo "  Touch ID: fingerprint TA set in the system image (fresh data volumes)"
	fi
	# callservicesd for the data volume (B, 2026-11-15): init copies it to /var/jb/redwood where its job runs it
	# (src/init.c seed_phone); select-system.py leaves framework executables out of the image, so it is added here
	CSD=System/Library/PrivateFrameworks/TelephonyUtilities.framework/callservicesd
	if [ -f $IOS/rootfs/$CSD ]; then mkdir -p sysroot/System/Library/Redwood/Phone; cp $IOS/rootfs/$CSD sysroot/System/Library/Redwood/Phone/callservicesd; chmod 755 sysroot/System/Library/Redwood/Phone/callservicesd
	else echo "build.sh: no $CSD in the iOS root: a fresh phone will have no callservicesd" >&2; fi
	# where the kernel mounts the iOS root container off the phone's flash (boot-arg apfs_volume=):
	# iOS has no /System/Volumes, and nothing can make a directory in this image once it is built
	mkdir -p sysroot/System/Volumes/iOS
	# the applications iOS installs on its first boot are system applications here (see select-system.py)
	if [ -d sysroot/private/var/staged_system_apps ]; then
		mv sysroot/private/var/staged_system_apps/* sysroot/Applications/
		rm -rf sysroot/private/var
	fi
	mv sysroot/sbin/launchd sysroot/sbin/launchd.ios
	cp obj/stage0 sysroot/sbin/launchd
	# the audio DSP's firmware, the phone's own from ../fw-audio (never in the repository), where the kernel reads
	# it once the root is mounted (bsd/dev/redwood_adsp.c, adsp=1): 20 MB that do not fit beside GL=1 in the small image
	if [ -f ../fw-audio/adsp.mdt ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/adsp.mdt ../fw-audio/adsp.b0[2-9] ../fw-audio/adsp.b[1-9][0-9] sysroot/fw/
	fi
	# x1slte: ABOX's Calliope DSP firmware (bsd/dev/redwood_exynos_sound.c, snd_calliope=1), the phone's /vendor/firmware
	# calliope_*.bin and abox_tplg.bin, from ../fw-sound (Samsung's: never committed). In the system image: 4 MB that do not
	# fit in the small one's 14.4 MB slot; the kernel reads them from /fw once iOS's volume is the root.
	for f in calliope_sram.bin calliope_dram.bin calliope_sram_2.bin calliope_dram_2.bin abox_tplg.bin; do
		if [ -f ../fw-sound/$f ]; then
			mkdir -p sysroot/fw
			install -m 0644 ../fw-sound/$f sysroot/fw/
		fi
	done
	# ... and the speaker amplifiers' configuration, the phone's /vendor/etc/aw882xx_acf.bin (bsd/dev/redwood_speaker.c,
	# for sound=3): Android's own register values, which are not in the repository either
	if [ -f ../fw-audio/aw882xx_acf.bin ]; then
		mkdir -p sysroot/fw
		cp ../fw-audio/aw882xx_acf.bin sysroot/fw/
	fi
	# the package manager's prefix, when tools/mk-jb.sh has fetched one: the Procursus bootstrap with
	# Sileo in it, as /jb. It cannot live at /var/jb in the image -- /private/var is a memory
	# filesystem made afresh at every boot -- so init copies it there (src/init.c, copy_tree).
	if [ -d ../jb/root/var/jb ]; then
		rm -rf sysroot/jb
		cp -a ../jb/root/var/jb sysroot/jb
		# the applications of the prefix are system applications here: LaunchServices builds its
		# database afresh at every boot by walking /Applications, which is how their icons appear
		for app in sysroot/jb/Applications/*.app; do
			[ -d "$app" ] && cp -a "$app" sysroot/Applications/
		done
		# Sileo moves, copies and removes files as root with /bin/mv, /bin/cp, /bin/rm, /bin/mkdir and
		# /bin/chmod -- where a jailbreak with a writable root keeps them -- and iOS's /bin has df and ps
		# alone: every one of those spawns failed with nothing said, and the package it had downloaded never
		# reached apt's archives ("Unable to fetch some archives"). The prefix's own programs, by those names.
		for c in $SILEO_BIN; do
			ln -sf /var/jb/usr/bin/$c sysroot/bin/$c
		done
		echo "  the package manager's prefix: $(du -sh sysroot/jb | cut -f1) as /jb (init puts it at /var/jb)"
	fi
	if [ -f "$IOS/rootfs/$CSD" ]; then
		mkdir -p sysroot/jb/redwood
		cp "$IOS/rootfs/$CSD" sysroot/jb/redwood/callservicesd
	fi
	# An application of this port's own, registered the same free way the prefix's are: a plain
	# directory in /Applications, which the LaunchServices database is rebuilt from at every boot
	# by walking that directory -- no installd, no container, no uicache. cp -a and not a link,
	# because a bundle of symbolic links is refused ("Unable to register") and the boot-time
	# registrar passes over mount points.
	#
	# This is in the SYSTEM image and not the small one, so a change to it costs a 2.5 GB rebuild
	# and a re-upload: SYSTEM=1 is what makes this line run again.
	# the triangle test app only on request (TRIANGLE=1): it did nothing for a user and sat on the home screen (A, 2026-11-20)
	if [ -n "${TRIANGLE:-}" ] && [ -d obj/RedwoodTriangle.app ]; then
		cp -a obj/RedwoodTriangle.app sysroot/Applications/
		echo "  RedwoodTriangle.app in /Applications (com.redwood.triangle, scheme redwoodtriangle:)"
	fi
	# Applications from outside this tree: every .app in ../apps, which is kept out of the
	# repository the way ../fw-* and ../jb are, so that nothing of somebody else's package can
	# reach it. They go into /Applications beside the port's own on exactly the same terms --
	# a plain directory, which LaunchServices finds by walking that folder at every boot.
	#
	# cp -aL and not cp -a: a bundle made of symbolic links is refused ("Unable to register"),
	# so whatever a link points at is copied as the file itself.
	for app in ../apps/*.app; do
		[ -d "$app" ] || continue
		cp -aL "$app" sysroot/Applications/
		echo "  ${app##*/} in /Applications ($(du -sm "$app" | cut -f1) MB from ../apps)"
	done
	# our audio device, where the audio server looks for such plug-ins (and for no others: Apple's are left out)
	mkdir -p sysroot/System/Library/Audio/Plug-Ins/HAL
	ln -s /redwood/lib/RedwoodAudio.driver sysroot/System/Library/Audio/Plug-Ins/HAL/RedwoodAudio.driver
	# ... and our Metal plug-in, at the one path Metal's own discovery can open: step 3 composes
	# "/System/Library/Extensions/" + MetalPluginName + ".bundle" and opens it with NSBundle
	# (docs/metal.md 1). Same shape as the audio plug-in above -- a link into /redwood/lib, where
	# the bundle itself travels in the small image -- because this directory is iOS's own and
	# nothing can be added to it after the image is built.
	#
	# This link is inert on a boot that does not ask for Metal: nothing composes that path until
	# MetalPluginName is on the IOAcceleratorES service, and only boot-arg metal=1 lets a process
	# put it there (iokit/Kernel/MiniIOMobileFramebuffer.cpp). So it is not behind a switch of its
	# own -- a switch here would cost a 2.5 GB system.apfs rebuild and a re-upload every time it
	# was flipped, and buy nothing a missing boot-arg does not already buy.
	mkdir -p sysroot/System/Library/Extensions
	ln -s /redwood/lib/RedwoodMetal.bundle sysroot/System/Library/Extensions/RedwoodMetal.bundle
	# the OS cryptex: on a device a separate volume grafted at /System/Cryptexes/OS. It holds the dyld
	# shared cache (dyld looks there first) and a few libraries and resources outside the cache.
	mkdir -p sysroot/System/Cryptexes
	if [ -d $IOS/cryptex ]; then python3 ../tools/release/copy_tree.py "$IOS/cryptex" sysroot/System/Cryptexes/OS; note_links "$IOS/cryptex" sysroot/System/Cryptexes/OS; fi
	# the App cryptex, the same way: Safari's daemons (eight launchd jobs run them from there), its settings and
	# the bundles around it, which the root's links reach (System/Library/PreferenceBundles/MobileSafariSettings.bundle...)
	if [ -d $IOS/cryptex-app ]; then python3 ../tools/release/copy_tree.py "$IOS/cryptex-app" sysroot/System/Cryptexes/App; note_links "$IOS/cryptex-app" sysroot/System/Cryptexes/App; fi
	D=sysroot/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld
	mkdir -p $D
	for f in $CACHE/dyld_shared_cache_arm64*; do
		case "$f" in *.symbols) continue ;; esac     # symbolication data, never mapped
		# -f: a cryptex fetched with the cache already in it would otherwise stop the build
		ln -f "$f" $D/
	done
	# A containing app gives the local provider its data/app-group containers.
	# Relocate rather than duplicate it: a metadata-only standalone registration
	# can otherwise win PlugInKit election over the working app extension.
	if [ -n "${JESVI_TARGET:-}" ]; then
		python3 ../tools/release/apply_mods.py "$JESVI_TARGET" system sysroot
	fi
	# Framework providers stay in their native paths. register_finder_host automatically prepares
	# a hidden containerized registration host from these bundles on the data volume; standalone
	# registerPlugin: loses entitlement/container metadata on this port. FILES_HOST=1 retains
	# the older image-time relocation option.
	if [ -n "${FILES_HOST:-}" ]; then
		python3 ../tools/storage/stage-provider.py sysroot obj/LocalFilesHost \
			$IOS/system.manifest obj/system-files.manifest
	else
		cp $IOS/system.manifest obj/system-files.manifest
	fi
	# TrollStore (opa334, MIT; the official 2.1.1 release, TrollStore.tar) for installing IPAs: com.redwood.trollstore
	# runs its own trollstorehelper install-trollstore once on a data volume without it (userland/src/trollstore_once.c)
	TS=${TROLLSTORE_TAR:-$HOME/ios-port-research/work/sideloading/TrollStore-uncompressed.tar}
	if [ -f "$TS" ]; then
		mkdir -p sysroot/System/Library/Redwood/TrollStore
		cp "$TS" sysroot/System/Library/Redwood/TrollStore/TrollStore.tar
		echo "  TrollStore: the official release in the image (installed at first boot)"
	fi
	# zlib where it saves room, as Apple keeps the root (a small file whole in its attribute): 5.4 GB of
	# files in 2.5 GB, what fits into the phone's RAM beside iOS
	python3 ../tools/apfs/mkapfs.py -z -o system.apfs System=sysroot,manifest=obj/system-files.manifest | tail -1
	[ -z "$CHECK" ] || ../tools/apfs/check-image.sh system.apfs System=sysroot,manifest=obj/system-files.manifest
	rm -rf sysroot
fi
if [ -n "${HACKIOS_IMAGE_CACHE:-}" ]; then
	python3 ../tools/release/system_cache.py store --ios "$IOS"
fi
fi
