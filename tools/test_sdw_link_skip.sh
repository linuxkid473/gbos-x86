#!/bin/sh
# Host tests for the Latitude 7320 SoundWire link 2 fix: the soundwire_intel option in usb/hwdetect.sh (DMI gate, MODOPTS
# override, written before MODOPTS) and kernel/isa_emu_patch/sdw_link_skip.py (applies to the 6.18 anchors, idempotent,
# aborts on a changed source).
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT HUP INT TERM
fail() { echo "FAIL: $*" >&2; exit 1; }

# ---- usb/hwdetect.sh ------------------------------------------------------------------------------------------------
log() { printf '%s\n' "$*" >> "$T/hwlog"; }
. "$ROOT/usb/hwdetect.sh"
GBOS_SYSFS_ROOT=$T/sys
mkdir -p "$T/sys/class/dmi/id"
printf 'Dell Inc.\n' > "$T/sys/class/dmi/id/sys_vendor"
printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
MODOPTS=
[ "$(gbos_sdw_link_opts 2>/dev/null)" = 'options soundwire_intel sdw_link_skip_mask=4' ] || fail "link 2 option not written"
grep -q 'sdw_link_skip_mask=4 (Latitude 7320 Detachable' "$T/hwlog" || fail "option not logged: $(cat "$T/hwlog")"
MODOPTS="xe.enable_psr=0 soundwire_intel.sdw_link_skip_mask=0"
[ -z "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "MODOPTS override not respected"
MODOPTS="soundwire-intel.sdw_link_skip_mask=0"
[ -z "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "MODOPTS override with a dash not respected"
MODOPTS="mt7921_common.disable_clc=1"
[ -n "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "unrelated MODOPTS suppressed the option"
MODOPTS=
printf 'Latitude 7320\n' > "$T/sys/class/dmi/id/product_name"
[ -z "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "option written on another Latitude"
printf 'Latitude 7320 Detachable\n' > "$T/sys/class/dmi/id/product_name"
printf 'LENOVO\n' > "$T/sys/class/dmi/id/sys_vendor"
[ -z "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "option written for another vendor"
rm -rf "$T/sys/class/dmi"
[ -z "$(gbos_sdw_link_opts 2>/dev/null)" ] || fail "option written without DMI"

# gbos_modopts writes it before its MODOPTS early return, and MODOPTS lines come after it (later options win).
sed -n '/^gbos_modopts() {/,/^}/p' "$ROOT/usb/hwdetect.sh" > "$T/modopts.sh"
awk '/gbos_sdw_link_opts >>/ { t = NR } /\[ -n "\$MODOPTS" \] \|\|/ { m = NR } END { exit !(t && m && t < m) }' "$T/modopts.sh" ||
  fail "gbos_modopts does not write the SoundWire option before MODOPTS"
grep -q 'sdw_link_skip_mask=$(cat /sys/module/soundwire_intel/parameters/sdw_link_skip_mask' "$ROOT/usb/hwdetect.sh" ||
  fail "hwdetect report does not show the parameter"

# ---- kernel/isa_emu_patch/sdw_link_skip.py ---------------------------------------------------------------------------
# The two anchors as they are in drivers/soundwire/intel_auxdevice.c (6.18.21).
mkdir -p "$T/src/drivers/soundwire"
cat > "$T/src/drivers/soundwire/intel_auxdevice.c" <<'EOF'
static int mclk_divider;
module_param_named(sdw_mclk_divider, mclk_divider, int, 0444);
MODULE_PARM_DESC(sdw_mclk_divider, "SoundWire Intel mclk divider");

static int sdw_master_read_intel_prop(struct sdw_bus *bus)
{
	fwnode_property_read_u32(link,
				 "intel-quirk-mask",
				 &quirk_mask);

	if (quirk_mask & SDW_INTEL_QUIRK_MASK_BUS_DISABLE)
		prop->hw_disabled = true;

	prop->quirks = SDW_MASTER_QUIRKS_CLEAR_INITIAL_CLASH |
		SDW_MASTER_QUIRKS_CLEAR_INITIAL_PARITY;
}
EOF
cp "$T/src/drivers/soundwire/intel_auxdevice.c" "$T/orig.c"
python3 "$ROOT/kernel/isa_emu_patch/sdw_link_skip.py" "$T/src" > "$T/p1"
[ "$(grep -c '^applied:' "$T/p1")" = 2 ] || fail "patch did not apply both edits: $(cat "$T/p1")"
F=$T/src/drivers/soundwire/intel_auxdevice.c
grep -q 'module_param_named(sdw_link_skip_mask, gbos_link_skip_mask, int, 0444);' "$F" || fail "parameter missing"
awk '/SDW_INTEL_QUIRK_MASK_BUS_DISABLE/ { q = NR } /gbos_link_skip_mask & BIT\(bus->link_id\)/ { g = NR }
     /prop->quirks = / { e = NR } END { exit !(q && g && e && q < g && g < e) }' "$F" || fail "hw_disabled check not after the BIOS quirk"
cp "$F" "$T/once.c"
python3 "$ROOT/kernel/isa_emu_patch/sdw_link_skip.py" "$T/src" > "$T/p2"
[ "$(grep -c '^already applied:' "$T/p2")" = 2 ] || fail "second run not idempotent: $(cat "$T/p2")"
cmp -s "$F" "$T/once.c" || fail "second run changed the file"
# A changed upstream source aborts and leaves the file alone.
sed 's/SoundWire Intel mclk divider/Intel mclk divider/' "$T/orig.c" > "$F"
cp "$F" "$T/changed.c"
if python3 "$ROOT/kernel/isa_emu_patch/sdw_link_skip.py" "$T/src" > "$T/p3" 2>&1; then fail "missing anchor did not abort"; fi
grep -q 'ANCHOR NOT FOUND' "$T/p3" || fail "abort message missing: $(cat "$T/p3")"
cmp -s "$F" "$T/changed.c" || fail "aborted run changed the file"
grep -q 'sdw_link_skip.py' "$ROOT/kernel/build_kernel.sh" || fail "build_kernel.sh does not apply sdw_link_skip.py"

echo "PASS: test_sdw_link_skip.sh"
