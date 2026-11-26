#!/bin/sh
# Fetch the firmware gbOS86 ships for generic Intel gen6+ laptops from kernel.org's linux-firmware and trim it
# to the chips the kernel #31 drivers cover. Output: usb/firmware/ (merged; existing files are kept).
#   tools/fetch_firmware.sh [WORKDIR]      (WORKDIR defaults to a temp dir; the clone is ~1.5 GB, the result ~100 MB)
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
W=${1:-$(mktemp -d)}
OUT=$REPO/usb/firmware

# IPU6 firmware for the in-tree driver (CONFIG_VIDEO_INTEL_IPU6).
# Keep this separate from linux-firmware: Intel publishes it in ipu6-camera-bins.
IPU6_BINS_COMMIT=30e87664829782811a765b0ca9eea3a878a7ff29
IPU6_FW_SHA256=70cbe88d075dc03938a94a897912409c5fdd56dc987b3db2a9a2a867bb2f5f99
IPU6_LICENSE_SHA256=6e25b8344c5e2b1d31b813387caee7d117188f75959b7b86d593f437a96755e6
IPU6_FW_CACHE=$W/ipu6-camera-bins-$IPU6_BINS_COMMIT
mkdir -p "$IPU6_FW_CACHE"
if [ ! -f "$IPU6_FW_CACHE/ipu6_fw.bin" ] ||
   [ "$(sha256sum "$IPU6_FW_CACHE/ipu6_fw.bin" | awk '{print $1}')" != "$IPU6_FW_SHA256" ]; then
  curl -fL -o "$IPU6_FW_CACHE/ipu6_fw.bin.tmp" \
    "https://raw.githubusercontent.com/intel/ipu6-camera-bins/$IPU6_BINS_COMMIT/lib/firmware/intel/ipu/ipu6_fw.bin"
  echo "$IPU6_FW_SHA256  $IPU6_FW_CACHE/ipu6_fw.bin.tmp" | sha256sum -c -
  mv "$IPU6_FW_CACHE/ipu6_fw.bin.tmp" "$IPU6_FW_CACHE/ipu6_fw.bin"
fi
if [ ! -f "$IPU6_FW_CACHE/LICENSE" ] ||
   [ "$(sha256sum "$IPU6_FW_CACHE/LICENSE" | awk '{print $1}')" != "$IPU6_LICENSE_SHA256" ]; then
  curl -fL -o "$IPU6_FW_CACHE/LICENSE.tmp" \
    "https://raw.githubusercontent.com/intel/ipu6-camera-bins/$IPU6_BINS_COMMIT/LICENSE"
  echo "$IPU6_LICENSE_SHA256  $IPU6_FW_CACHE/LICENSE.tmp" | sha256sum -c -
  mv "$IPU6_FW_CACHE/LICENSE.tmp" "$IPU6_FW_CACHE/LICENSE"
fi
mkdir -p "$OUT/intel/ipu"
if [ -f "$OUT/intel/ipu/ipu6_fw.bin" ] &&
   [ "$(sha256sum "$OUT/intel/ipu/ipu6_fw.bin" | awk '{print $1}')" != "$IPU6_FW_SHA256" ]; then
  echo "existing $OUT/intel/ipu/ipu6_fw.bin does not match the pinned Intel firmware; preserving it" >&2
  exit 1
fi
if [ -f "$OUT/intel/ipu/LICENSE" ] &&
   [ "$(sha256sum "$OUT/intel/ipu/LICENSE" | awk '{print $1}')" != "$IPU6_LICENSE_SHA256" ]; then
  echo "existing $OUT/intel/ipu/LICENSE does not match the pinned Intel license; preserving it" >&2
  exit 1
fi
[ -f "$OUT/intel/ipu/ipu6_fw.bin" ] || cp "$IPU6_FW_CACHE/ipu6_fw.bin" "$OUT/intel/ipu/ipu6_fw.bin"
[ -f "$OUT/intel/ipu/LICENSE" ] || cp "$IPU6_FW_CACHE/LICENSE" "$OUT/intel/ipu/LICENSE"
[ -d $W/fw/.git ] || git clone --depth 1 --no-checkout https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git $W/fw
cd $W/fw
git ls-tree -r --name-only HEAD > $W/all.txt
git rev-parse HEAD > $W/commit.txt
# newest N files per family (sorted by name, so the highest API number wins); iwlwifi falls back to older ones.
g() { grep "$@" $W/all.txt || true; }
{
  # Intel Wi-Fi: keep the two newest API versions of every chip family (ucode + pnvm)
  g -E '^intel/iwlwifi/' | sed -E 's#^(.*/)(iwlwifi-[^.]*[a-z0-9])-([0-9]+)\.(ucode)$#\1\2 \3 &#' | awk 'NF==3{fam=$1; n[fam]++; f[fam,n[fam]]=$3; v[fam,n[fam]]=$2} END{for(k in n){ # keep last two by numeric order
      m=n[k]; for(i=1;i<=m;i++) for(j=i+1;j<=m;j++) if (v[k,j]+0 < v[k,i]+0) {t=v[k,i];v[k,i]=v[k,j];v[k,j]=t;t=f[k,i];f[k,i]=f[k,j];f[k,j]=t}
      for(i=(m>2?m-1:1);i<=m;i++) print f[k,i]}}'
  g -E '^intel/iwlwifi/.*\.pnvm$'
  # Intel Bluetooth
  g -E '^intel/ibt-'
  # i915 GuC/HuC/DMC (skl..mtl)
  g -E '^i915/'
  # SOF audio firmware + topologies
  g -E '^intel/(sof|sof-ipc4|sof-ace-tplg|sof-tplg)'
  # Realtek Wi-Fi/BT
  g -E '^rtw88/(rtw8822b|rtw8822c|rtw8723d|rtw8821c)_'
  g -E '^rtw89/rtw(8852a|8852b|8852bt|8852c|8851b|8922a)_fw'
  g -E '^rtl_bt/'
  # MediaTek mt7921/22/25 Wi-Fi + BT
  g -E '^mediatek/(WIFI_|BT_RAM_CODE_MT79|mt7925/|mt7927/)'
  # Qualcomm: ath10k QCA6174/QCA9377, ath11k QCA6390/WCN6855, QCA BT
  g -E '^ath10k/(QCA6174|QCA9377)/'
  g -E '^ath11k/(QCA6390|WCN6855)/'
  g -E '^qca/(rampatch_usb|nvm_usb|rampatch_0|nvm_0|htbtfw|crbtfw|crnv|QCA|rampatch_|nvm_)'
  # Broadcom Bluetooth + brcmfmac PCIe
  g -E '^brcm/(BCM|brcmfmac4(3602|350|356|358|3455|371))'
  # Marvell 88W8897 Wi-Fi + Bluetooth (Surface Pro 4/5/6, Book 1/2, Laptop 1/2; mwifiex_pcie). Without it the Surface Pro 5 had
  # "Direct firmware load for mrvl/pcie8897_uapsta.bin failed with error -2": no Wi-Fi, and no Bluetooth either (the same firmware
  # starts the chip's USB Bluetooth function).
  g -E '^mrvl/pcie8897_uapsta\.bin$'
  # Cirrus amps found on some laptops
  g -E '^cirrus/cs35l(41|56)-[^/]*$'
  # Texas Instruments TAS2781/TAS2563 smart amps (Lenovo ThinkBook/Yoga/Legion, Meteor Lake+; I2C amps behind an HDA codec). The HDA driver
  # (snd-hda-scodec-tas2781-i2c) asks for "TAS2XXX<codec subsystem id>.bin" / "TAS2XXX<id><n>.bin" (and older "TIAS2781RCA<n>.bin") with NO
  # directory and no MODULE_FIRMWARE entry, so fw_wanted.py cannot see them; linux-firmware keeps them under ti/tas2781/ (and some at the
  # root). Take both places; the ones under ti/ are also copied to the root below, where the loader looks.
  g -E '^(TAS2XXX|TIAS2781RCA|INT8866RCA)[^/]*\.bin$'
  g -E '^ti/(tas2781|tas25xx|tas2563)/'
  # Drivers that build the firmware name at run time (not in MODULE_FIRMWARE): ship their whole family
  g -E '^ath12k/WCN7850/'
  g -E '^ath10k/(QCA6174|QCA9377|QCA9984|QCA9887|QCA988X)/'
  g -E '^(rtw88|rtl_bt|rtl_nic|qca|brcm|cypress|cirrus|xe)/'
  g -E '^intel/ish/'
  # Realtek RT1320 SoundWire amplifier (Surface Laptop 7): MCU patch code requested at probe, no MODULE_FIRMWARE entry
  g -E '^realtek/rt1320/'
  g -E '^rtw89/'
  # Intel sensor hub (ISH): the BIOS can also load it, the files in intel/ish/ are for boards that need the OS to.
  # Everything the shipped kernel modules ask for (MODULE_FIRMWARE read from the .ko files): amdgpu, r8169/r8152, btmtk, mt76, ...
  git show HEAD:WHENCE > $W/WHENCE
  python3 -I $REPO/tools/fw_wanted.py $REPO/usb/modules $W/all.txt $W/WHENCE
} | grep -v ' ' | sort -u > $W/want.txt
python3 -I $REPO/tools/fw_wanted.py $REPO/usb/modules $W/all.txt $W/WHENCE --unresolved > $W/unresolved.txt
echo "modules ask for $(wc -l < $W/unresolved.txt | tr -d ' ') names linux-firmware does not have (see $W/unresolved.txt; older/pre-production chips, SDIO boards)"
tr '\n' '\0' < $W/want.txt | xargs -0 git checkout HEAD --
mkdir -p $OUT
tar -cf - -T $W/want.txt | tar -xf - -C $OUT
# TAS2781/TAS2563 HDA amp firmware is requested from the firmware root ("TAS2XXX38A5.bin", no ti/tas2781/ prefix in the module's format
# strings): copy whatever linux-firmware keeps under ti/ to the root as well (existing root files win).
for d in $OUT/ti/tas2781 $OUT/ti/tas25xx $OUT/ti/tas2563; do
  [ -d $d ] || continue
  for f in $d/TAS2XXX*.bin $d/TIAS2781RCA*.bin $d/INT8866RCA*.bin; do
    [ -f "$f" ] && { [ -f "$OUT/${f##*/}" ] || cp "$f" "$OUT/${f##*/}"; }
  done
done
# WHENCE "Link: a -> b" entries (b relative to a's folder) exist only after linux-firmware's own install script runs; they are not in
# the git tree. Drivers still ask for the link names: Intel AX201 Bluetooth wants intel/ibt-0040-1050.sfi, a link to ibt-0040-0041.sfi
# (reporter's hwdetect: "Failed to load Intel firmware file intel/ibt-0040-1050.sfi (-2)"). FAT has no symlinks: copy the target,
# but only when we ship the target and the link name isn't a real file already.
git show HEAD:WHENCE | sed -n 's/^Link: \([^ ]*\) -> \(.*\)$/\1 \2/p' | while read l t; do
  src=$OUT/$(dirname "$l")/$t
  [ -f "$OUT/$l" ] || [ ! -f "$src" ] || { mkdir -p "$OUT/$(dirname "$l")"; cp "$src" "$OUT/$l"; }
done
# This kernel's iwlwifi requests "iwlwifi-*.ucode" from the firmware root (no intel/iwlwifi/ prefix).
if [ -d $OUT/intel/iwlwifi ]; then mv $OUT/intel/iwlwifi/* $OUT/ && rmdir $OUT/intel/iwlwifi; fi
# keep the symlink-targets: linux-firmware uses symlinks (e.g. in rtw89, intel/sof); FAT can't, so dereference.
find $OUT -type l | while read l; do t=$(readlink -f "$l"); [ -f "$t" ] && { rm "$l"; cp "$t" "$l"; } || rm "$l"; done
echo "linux-firmware commit: $(cat $W/commit.txt)" > $OUT/FIRMWARE_SOURCE.txt
echo "ipu6-camera-bins: commit $IPU6_BINS_COMMIT, ipu6_fw.bin sha256 $IPU6_FW_SHA256" >> "$OUT/FIRMWARE_SOURCE.txt"
du -sh $OUT

# Intel SOF audio firmware + topologies come from a separate repo (not in linux-firmware).
SOFV=2026.09.2
if [ ! -d $W/sof-bin-$SOFV ]; then
  curl -fL -o $W/sof.tgz https://github.com/thesofproject/sof-bin/releases/download/v$SOFV/sof-bin-$SOFV.tar.gz
  tar -xzf $W/sof.tgz -C $W
fi
mkdir -p $OUT/intel
for d in sof sof-ipc4 sof-ipc4-lib sof-ipc4-tplg sof-tplg; do
  rm -rf $OUT/intel/$d; cp -RL $W/sof-bin-$SOFV/$d $OUT/intel/$d   # -L: FAT has no symlinks
  find $OUT/intel/$d -name '*.ldc' -delete                            # debug dictionaries, not needed
done
# The kernel looks for the HDA-generic topologies of Meteor Lake and newer (ACE) in intel/sof-ace-tplg ("Topology file: intel/sof-ace-tplg/
# sof-hda-generic-2ch.tplg ... not found", Dell Inspiron 14 Plus 7440, issue 17). linux-firmware has that folder as a symlink to
# sof-ipc4-tplg, which FAT cannot hold: ship copies of the generic ones.
rm -rf $OUT/intel/sof-ace-tplg; mkdir -p $OUT/intel/sof-ace-tplg
cp $OUT/intel/sof-ipc4-tplg/sof-hda-generic*.tplg $OUT/intel/sof-ace-tplg/
cp $W/sof-bin-$SOFV/LICENCE.Intel $OUT/intel/LICENCE.sof-bin
echo "sof-bin: v$SOFV" >> $OUT/FIRMWARE_SOURCE.txt
du -sh $OUT
