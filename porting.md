# Porting gbOS86 to a new Google build

Google's lapis build is pinned in [`lapis/BUILD`](lapis/BUILD) (currently `16693588`, `CL3B.260622.276.R5`). A new build changes the
files our patches were made from. This page is the whole procedure; the tools do the mechanical parts and say what a person
has to look at.

```sh
tools/check_new_build.sh            # is there a newer lapis build? (also: tools/check_new_build.sh <build id you heard about>)
# edit lapis/BUILD (BUILD_ID, VERSION, URL) and lapis/recovery.zip.sha256, then:
rm -rf lapis/parts && ./build.sh --dry-run   # build.sh downloads the new recovery.zip and fills lapis/parts (6 partitions)
tools/port_build.sh --isa           # DRY RUN: re-patch, check anchors, scan for unsupported instructions. Touches nothing.
tools/port_build.sh --write --product   # when it says 0 FAIL: writes usb/patched/*, usb/libkm_compat.so, usb/system_ext_manifest.xml, usb/patched/MANIFEST
sh tools/test_port_scripts.sh       # the boot scripts' patch logic under the stick's busybox: PORT_SCRIPTS_PASS
tools/deploy_stick.sh               # then boot the stick, choose 1 (Update), read stage0-dmesg.txt (below)
```

`PORT_KEEP=1 tools/port_build.sh ...` keeps the unpacked build in a docker volume (`gbos-port-work`), so re-runs skip the unpacking.
`docker volume rm gbos-port-work` forgets it.

## What is generated, and how it finds its place

Nothing is located by address or pinned by hash in a script any more. Patchers find their site by pattern or exported symbol,
and the scripts on the stick read [`usb/patched/MANIFEST`](usb/patched/MANIFEST), which `port_build.py` writes.

| Substitution | Tool | How it finds the site | If it finds nothing |
|---|---|---|---|
| `libz.so` (system, vendor): `adler32_z` never calls its AVX-VNNI helper | `tools/patch_libz_adler.py` | the `cmp $0x40,%rdx; jae` gate, `cmpl $0,x86_cpu_enable_simd; je; call` and the scalar entry bytes | exit 3; read the new `adler32_z` in `objdump -d` and adjust `GATE` |
| `libcrypto.so` (system, vendor, every APEX copy of the Conscrypt hash): SHA-NI block functions jump to the scalar ones | `tools/sha_redirect.py` | every `sha*` instruction, back to the function prologue, and the scalar sibling's prologue | exit 3 with `UNMATCHED`; add the new prologue to `FAMILIES` |
| `libkm_compat.so`: software KeyMint for every security level | `tools/patch_km_compat.py` | the exported `KeystoreCompatService::getKeyMintDevice` symbol, the first `mov %edx,%reg` in its prologue becomes `xor reg,reg` | exit 3; disassemble `getKeyMintDevice`, the level is argument 2 (`edx`) |
| `system_ext_manifest.xml`: VINTF without the Trusty KeyMint/RKP/SecureClock `<hal>`s | `port_build.py` | removes those three blocks from Google's own manifest, so it is always in sync with the build | WARN names the HAL that is no longer declared |

On the device, `hook.sh` (`gbos_bind_patches`), `early_patch.sh` and `apex_patch.sh` bind a patched file only over a file whose sha256 is
the one recorded in the MANIFEST, so a stale patch is never bound over a different library. They now log why when they do not:

```
hook: /new/system/lib64/libz.so NOT patched (original is 77f1..., patch was made for 371f...: run tools/port_build.sh)
hook: PATCH-STATUS 4 applied, 1 not applied
```

`PATCH-STATUS N applied, 0 not applied` in `stage0-dmesg.txt` is the one line that says a build is fully patched.
`apex_patch: ... left alone (sha256 ...)` lists APEX copies of libcrypto that no patch covers.

## Reading the `port_build` output

* `OK ... identical to the shipped file`: the patcher reproduces what is already in `usb/` (the proof that the tool, not a
  lucky hand edit, makes our patches; on builds 16452207, 16658739 and 16693588 all six patches and the VINTF manifest match byte for byte).
* `FAIL ...`: a patch could not be made. Nothing is written. Fix the patcher's pattern (the table above), run again.
* `WARN anchor GONE: ...`: a file, line or property the boot scripts assume has moved or been renamed. Each line says what
  breaks and lists similar paths. Typical case: Google renames a Trusty-backed service's VINTF fragment, so `hook.sh` no longer hides
  it and boot hangs on it ("Waited one second for ..."): update the path in `hook.sh` and in `port_build.py`'s `anchors()` together.
* `WARN watched file ... CHANGED`: a file our overlay was copied from changed in Google's build (the audio HAL config,
  `sof-soundwire_*` configs, the vold fstab). Compare it with `usb/audio/` / the `hook.sh` code, merge the difference, and
  re-run with `--write` to record the new baseline.
* `FAIL isa ...: NOT emulated`: the build uses an instruction family the kernel emulator lacks (AVX-512, AMX, ...). It needs an
  emulator addition (`kernel/isa_emu_patch/`, kernel rebuild) or a CPU-feature gate, as for the existing ones. Instructions listed
  `OK ... emulated` need nothing.
* `INFO other APEX libcrypto ...`: libcrypto copies that contain SHA-NI but are not patched today (builds 16452207, 16658739 and 16693588: the
  clearkey, crosfp and StrongBox APEXes). They are rarely used here; to patch one, add an `apex` target to `port_build.py`.

## What no tool can check (needs the laptop)

1. SELinux: `gbos.rc` names `u:r:shell:s0` and `u:r:hal_gatekeeper_default:s0`; a policy change would make init refuse a service.
2. First-stage init expectations of the new build (dm-linear layout, `super.layout`, bootdisk/vbmeta faking).
3. New Trusty-backed or GSC-backed services that did not exist before (they show up as a hang waiting on one).
4. Kernel: the vendor/system DLKMs are for Google's kernel and are not used, but the userspace may need newer kernel features
   than our android17-6.18 tree (`port_build` prints Google's kernel version for comparison).

Boot once, choose **1 (Update)** from the stick on the installed system to collect the logs, and read, in this order:
`stage0-dmesg.txt` (`PATCH-STATUS`, `hook:` lines, `FATAL`), `kmsg.log` (confirm `Linux version ... #N`), `logcat.txt` (crash loops, `drmhwc`).
Data carries over to a newer build; going back to an older one needs a data wipe.

## After a successful port

* Commit `usb/patched/`, `usb/libkm_compat.so`, `usb/system_ext_manifest.xml`, `lapis/BUILD`, `lapis/recovery.zip.sha256`.
* Add a dated line to `HANDOFF.md` (build id, what the tool reported, what the laptop showed).
* `usb/make_super.py`, `tools/make_gbos86_image.sh` and the installer need no change: they take whatever is in `lapis/parts`.
