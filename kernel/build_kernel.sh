#!/bin/sh
# Build the gbOS86 kernel: AOSP android17-6.18 + our patches + KernelSU, in the gbos-kbuild
# container, with the sources in the docker volume gbos-ksrc (kernel/fetch_kernel_sources.sh).
#
#   sh kernel/build_kernel.sh [BASE_CONFIG] [JOBS]
#
#   BASE_CONFIG  a kernel .config to start from. Default: the one embedded in the shipped kernel
#                (usb/out/BOOTX64.EFI), extracted with tools/extract_ikconfig.py. The upstream
#                source is Google's recovery kernel config (README "Building from scratch").
#   JOBS         parallel compile jobs (default: nproc)
#
# What it does:
#   1. applies the patches (isa_emu, ashmem, i915 colours, SELinux, Surface, ALC269 quirks, SoundWire) and
#      KernelSU (kernel/kernelsu/apply.sh)
#   2. writes the config (kernel/configure_whl.py --generic), the kernel command line and the
#      initramfs path, then olddefconfig
#   3. builds bzImage + modules
#   4. modules_install -> the stage0 initramfs (kernel/stage0_modules.txt) -> rebuild, so the
#      initramfs is embedded in the kernel
#   5. copies bzImage to usb/out/BOOTX64.EFI, the modules to usb/modules/ and the Bluetooth modules
#      to usb/kmod/
#
# The kernel image is the bootable kernel (BOOTX64.EFI): copy it to both EFI copies on the stick
# (EFI/BOOT/ and GBOS/EFI/BOOT/) and pick "1 Update" to install it on the internal disk.
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
JOBS=${2:-$(nproc)}
VOL=${GBOS_KERNEL_VOLUME:-gbos-ksrc}
C=${CLANG_DIR:-/build/inputs/clang-r584948c}

# The base config: the one embedded in the shipped kernel, unless one is given.
BASE=${1:-}
W=$(mktemp -d "${TMPDIR:-/tmp}/kcfg.XXXXXX"); trap 'rm -rf "$W"' EXIT
if [ -z "$BASE" ]; then
  BASE=$W/base.config
  python3 "$ROOT/tools/extract_ikconfig.py" "$ROOT/usb/out/BOOTX64.EFI" > "$BASE" \
    || { echo "no base config: pass one, or extract it with tools/extract_ikconfig.py"; exit 1; }
  echo "base config: extracted from $(basename "$ROOT/usb/out/BOOTX64.EFI")"
else
  echo "base config: $BASE"
  cp "$BASE" "$W/base.config"
  BASE=$W/base.config
fi

docker run --rm -i --platform linux/amd64 -v "$VOL":/build -v "$ROOT":/host:ro -v "$W":/cfg gbos-kbuild bash -s <<EOF
set -e
[ -x $C/bin/clang-22 ] || { echo "missing clang-22 in $C (set CLANG_DIR to an installed toolchain)" >&2; exit 1; }
export PATH=$C/bin:\$PATH LD_LIBRARY_PATH=$C/lib:\$LD_LIBRARY_PATH
# KernelSU reads its version from its own git checkout; the volume's files are root's, the
# checkout on the host is not, so git would refuse it without this.
git config --global --add safe.directory /build/inputs/KernelSU
mkdir -p /build/out
# Continue the kernel build number ("Linux version ... #N"): the build increments the .version
# file in the build directory, so the number of the kernel that is being replaced goes in.
if [ -f /host/usb/out/BOOTX64.EFI ]; then
  prev=\$(grep -a -m1 -o '#[0-9]\+ ' /host/usb/out/BOOTX64.EFI | head -1 | tr -dc '0-9')
  [ -n "\$prev" ] && echo "\$prev" > /build/out/.version && echo "kernel build number: #\$((prev + 1))"
fi
cd /build/src
# Pin the kernel release to "6.18.21-gbos-whl": without this, scripts/setlocalversion appends "+" when the tree is a (dirty) git checkout
# and nothing when it is a tarball, so the release (and the module directory the stick needs) depended on how the sources were unpacked.
: > .scmversion
# ... but this kernel's scripts/setlocalversion ignores .scmversion (it prints "+" for any git checkout past the tag): make it stop there.
sed -i 's|^\\tif ! head=\\\$(git rev-parse --verify HEAD 2>/dev/null); then|\\tif test -e "\\\$srctree/.scmversion" \\|\\| ! head=\\\$(git rev-parse --verify HEAD 2>/dev/null); then|' scripts/setlocalversion
grep -q 'srctree/.scmversion' scripts/setlocalversion || { echo "setlocalversion patch did not apply" >&2; exit 1; }

# 1. the patches
export P=/host/kernel/isa_emu_patch
sh \$P/apply.sh
P=/host/kernel/ashmem_shim sh /host/kernel/ashmem_shim/apply.sh
for s in i915_color_pipeline i915_edp_colorspace i915_edp_no_colorspace i915_edp_full_range \\
         i915_ignore_gamma debug_selinux surface_sl7_sam alc269_hp_envy13_aq alc269_infinix_inbook_x3; do
  sh \$P/\$s.sh > /dev/null
done
python3 \$P/sdw_sl7.py /build/src > /dev/null
python3 \$P/sdw_link_skip.py /build/src > /dev/null
python3 /host/kernel/isa_emu_patch/camera_7320.py /build/src
P=/host/kernel/kernelsu sh /host/kernel/kernelsu/apply.sh | tail -2
P=/host/kernel/surface sh /host/kernel/surface/apply.sh

# 2. the config
cp /cfg/base.config /build/out/.config
python3 /host/kernel/configure_whl.py --generic /build/out/.config /build/out/.config.new
cp /build/out/.config.new /build/out/.config
/build/src/scripts/config --file /build/out/.config \\
  --set-str CMDLINE "\$(cat /host/kernel/isa_emu_patch/generic_cmdline.txt)" \\
  --set-str INITRAMFS_SOURCE ""
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 olddefconfig > /dev/null

# 3. the kernel and the modules
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 -j$JOBS bzImage modules

# 4. the stage0 initramfs (busybox + init + the modules stage0 loads) and the kernel with it
rm -rf /build/modroot_all
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 \\
  modules_install INSTALL_MOD_PATH=/build/modroot_all INSTALL_MOD_STRIP=1 > /dev/null
sh /host/kernel/make_stage0_initramfs.sh /build/modroot_all /build/out/gbos_stage0.cpio
/build/src/scripts/config --file /build/out/.config --set-str INITRAMFS_SOURCE /build/out/gbos_stage0.cpio
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 olddefconfig > /dev/null
make -C /build/src O=/build/out ARCH=x86_64 LLVM=$C/bin/ CC=$C/bin/clang-22 -j$JOBS bzImage
echo "BUILD OK"
EOF

# 5. the results
mkdir -p "$ROOT/usb/out" "$ROOT/usb/kmod"
docker run --rm -i --platform linux/amd64 -v "$VOL":/build -v "$ROOT":/host gbos-kbuild bash -s <<EOF
set -e
cp /build/out/arch/x86/boot/bzImage /host/usb/out/BOOTX64.EFI
# The module tree goes to usb/modules/lib/modules/<uname -r>/, the layout the release tarball
# has and the one usb/hwdetect.sh expects (it binds gbos/modules/lib/modules over /lib/modules).
rm -rf /host/usb/modules
mkdir -p /host/usb/modules/lib
cp -a /build/modroot_all/lib/modules /host/usb/modules/lib/
rm -f /host/usb/modules/lib/modules/*/build
# usb/hwdetect.sh binds gbos/modules/lib/modules/\$(uname -r): the directory name has to be the running kernel's release.
rel=\$(cat /build/out/include/config/kernel.release)
[ -f /host/usb/modules/lib/modules/\$rel/modules.dep ] || { echo "module tree is not usb/modules/lib/modules/\$rel" >&2; exit 1; }
# Include the kernel and actual module tree so added or changed modules trigger deployment.
sh /host/kernel/module_build_id.sh /host/usb/out/BOOTX64.EFI /host/usb/modules/lib/modules/\$rel > /host/usb/modules/lib/modules/\$rel/gbos-build-id
# the files were written by root in the container: give them back to the user
chown -R $(id -u):$(id -g) /host/usb/modules /host/usb/kmod /host/usb/out/BOOTX64.EFI
for m in btusb btintel btrtl; do
  f=\$(find /build/out/drivers/bluetooth -name "\$m.ko" | head -1)
  [ -n "\$f" ] && cp "\$f" /host/usb/kmod/\$m.ko
done
EOF
echo "kernel: usb/out/BOOTX64.EFI ($(wc -c < "$ROOT/usb/out/BOOTX64.EFI" | tr -d " ") bytes)"
echo "modules: $(find "$ROOT/usb/modules" -name '*.ko' | wc -l) in usb/modules/"
echo
echo "Test it:  kernel/isa_emu_test/run_isa_qemu.py usb/out/BOOTX64.EFI   (expects ISA_EMU_TEST_PASS)"
echo "Ship it:  copy usb/out/BOOTX64.EFI to both EFI copies on the stick, then '1 Update'."
