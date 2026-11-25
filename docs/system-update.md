# System updates without the USB stick

An installed gbOS86 (internal NVMe, eMMC or SATA disk) can move to a newer **system set** without booting the stick: a new
Google build (`super.img`), our scripts and patches, firmware, modules and the kernel, all from one gbOS86 commit. Apps, accounts
and settings (`/data`) are kept.

The update arrives as a **bundle**: a plain folder named `gbos86-update-<number>-<id>`, signed by the project's key. The maintainer
publishes bundles on the project's GitHub releases with one command, and the **gbOS86 Updater** app on the laptop finds them (it checks
after each start and tells you with a notification), downloads them and hands them over. No second computer is needed. Nothing in
Android installs it. It is only copied onto the system partition (GBOSSYS), and the next start checks it again, before Android
runs, and only then switches it in. If the new system does not finish starting three times, the previous one comes back by itself.

Code: [`usb/sysupdate.sh`](../usb/sysupdate.sh) (the laptop side), [`tools/make_system_update.sh`](../tools/make_system_update.sh)
(making bundles), [`tools/publish_system_update.sh`](../tools/publish_system_update.sh) (publishing them on GitHub),
[`tools/verify_system_update.sh`](../tools/verify_system_update.sh) (checking them on a PC), the app in [`updater-app/`](../updater-app).
Tests: `sh tools/test_system_update.sh` must print `SYSTEM_UPDATE_TEST_PASS`, `sh tools/test_updater_core.sh` `UPDATER_CORE_TEST_PASS`.

## Updating a laptop

You need a bundle folder made by the project (below) and an installed gbOS86 that has `gbos/sysupdate.sh` and the project's public key
in `gbos/sysupdate.pub`. An install gets both from the stick (install or menu **1 Update**) once the stick has them.

Get the bundle onto GBOSSYS in one of these ways. All of them only put files into `gbos/update.inbox/` on GBOSSYS:

1. **The gbOS86 Updater app, from GitHub** (the usual way). After each start it looks for a published update and shows a
   notification "gbOS86 update N available". Open the app, tap **Download and install**, then **Restart now** when it says the update
   is ready. Details: [The app](#the-app).
2. **The app, from a folder** you copied to the laptop yourself (for example to Downloads): Choose update folder, Copy this folder to
   the system, Restart now.
3. **From Windows** (dual-boot): GBOSSYS has a drive letter. Copy the whole `gbos86-update-...` folder into `gbos\update.inbox\`.
4. **From a root shell in gbOS86** (KernelSU):
   `su -c "/postinstall/busybox sh /postinstall/gbos/sysupdate.sh stage /sdcard/Download/gbos86-update-..."`
   (`stage` also takes the bundle as one `.tar`; `cancel` removes waiting bundles; `status` shows the state).

Then restart. The start takes about a minute longer (every file of the new set is hashed), and the screen says what happens. Afterwards
`gbos/update/status.txt` on GBOSSYS says what happened, `gbos/update/history.txt` keeps the last 200 results, and a refused or rolled-back
bundle leaves its manifest and a `reason.txt` in `gbos/update.rejected/` (its files are deleted to free the space).

**If the new system does not finish starting**, just switch the laptop on again. After three starts that do not reach the Android
desktop, the previous system is put back. `SYSUPDATE_ATTEMPTS=N` in `gbos.cfg` changes the three. One exception: when the update
brought a **new Google build** and Android already ran it, Android may have upgraded `/data`, and the older build may not start
with it. Then the laptop stays on the new set and `status.txt` says so. Use the stick's menu 1, or create an empty file
`gbos/update/rollback-request` on GBOSSYS (from Windows or the stick) to go back anyway.

**Space.** GBOSSYS is 8 GiB and holds one system set (about 4.5 GB) plus logs. A bundle carries only the files that changed, but a
new Google build includes the 3.7 GB `super.img`, which needs about 3.8 GB free next to the current set. If it does not fit, the
app (tick "delete old boot logs") or `stage --prune-logs` deletes the oldest `logs/boot-N` folders, never the newest three. Or copy
the logs off and delete them yourself, or update with the stick's menu 1. Nothing deletes logs unless you ask.

**Settings in `gbos.cfg`:** `SYSUPDATE=0` stops the laptop from looking at `update.inbox` (an update already in progress still finishes
or rolls back); `SYSUPDATE_ATTEMPTS=N` (default 3).

## The signing key (maintainer)

```sh
tools/make_system_update.sh keygen /media/offline-stick/gbos86-key   # asks for a passphrase; refuses a folder inside a git repo
cp /media/offline-stick/gbos86-key/sysupdate.pub usb/sysupdate.pub   # the PUBLIC key: commit it
```

* `sysupdate-private.pem` is RSA-3072, encrypted with the passphrase (AES-256). Keep it on an offline stick, with a second copy
  somewhere else, and only plug it in to sign. Never copy it into the repository, a cloud folder or the laptop. `--no-passphrase` is
  for tests.
* `usb/sysupdate.pub` holds only the modulus (`gbos86-sysupdate-key 1` / `rsa3072 <768 hex digits>`). The repository ships a
  placeholder with no key, and a laptop with the placeholder refuses every bundle, so nothing can be installed before you commit the
  real public key and it reaches the laptops (stick install or menu 1).
* **Changing the key:** sign a bundle with the old key whose set carries the new public key (`build --pub new.pub`). After it is
  installed, sign with the new key. A lost private key means every laptop needs one stick menu-1 update with a new public key.

## Making a bundle (maintainer)

```sh
# full bundle: every file of the set
tools/make_system_update.sh build --key KEY.pem --tree /Volumes/GBOS/gbos --commit v1.2
# delta: only what changed since the set the laptops have (a system.manifest or the previous bundle's manifest)
tools/make_system_update.sh build --key KEY.pem --tree /Volumes/GBOS/gbos --commit v1.3 --base build/system-updates/gbos86-update-.../manifest
tools/verify_system_update.sh build/system-updates/gbos86-update-...   # the device code under busybox, and openssl
```

* **The set** is `install.sh`'s `FILES` and `DIRS`, read from that commit's own `install.sh`. A file tracked in git comes from git
  (`--commit REV` uses `git archive`, `--worktree` the checkout and adds `-dirty` to the commit if `usb/` has changes). Everything else
  (`super.img`, `super.layout`, firmware blobs, `modules/`, `ksu/`) comes from `--tree`, a stick-like `gbos/` folder. The kernel is
  `usb/out/BOOTX64.EFI` of the commit (`--kernel` overrides).
* `camera/` is left out unless `--with-dir camera`: it is calibration extracted from one laptop's Windows files. A laptop keeps its
  own `camera/` because a set that does not mention a top-level folder never touches it.
* **The Google build** comes from `patched/MANIFEST`'s `# build` line. It must match `lapis/BUILD` and, when the `gbos-erofs` docker
  image exists, `super.img`'s own `build.prop` (`dump.erofs`). The laptop checks `super.img` again before installing it.
* `--seq N` is the update number (default: UTC `yyyymmddHHMM`); a laptop only takes a higher one. `--tar` also writes one `.tar`
  file. The bundle is written to `build/system-updates/` (gitignored).
* **The stick's own set:** `tools/make_system_update.sh manifest --tree /Volumes/GBOS/gbos --kernel /Volumes/EFI/EFI/BOOT/BOOTX64.EFI
  --out /Volumes/GBOS/gbos/system.manifest` describes exactly what is on the stick (unsigned: it only names the set). Menu 1 copies it to
  the laptop when its build-tied hashes match the stick's files. `deploy_stick.sh` deletes a stick's `system.manifest` when it changes
  files, because the manifest would no longer describe the stick.

## Releasing from main (maintainer): `tools/release.sh`

Only for the main repository (`REPO` in `tools/release.conf`): a clone whose `origin` is a fork or the lab repository is refused.
Nothing is uploaded until every check passes.

```sh
tools/release.sh setup          # once per computer: where the signing key and the stick folder are (~/.config/gbos86/release.conf)
tools/release.sh start 2026-11-20   # the starting point (tools/release.conf): commit and push it
tools/release.sh                # the plan: what a release made today contains, which laptops get a small download
tools/release.sh check          # every check, nothing signed or uploaded
tools/release.sh publish        # the checks, then sign and upload (asks first); --dry-run makes the files only
```

**The run day and the starting point.** An update is always `main` as it is on the day the script runs; its number is that day's
date and time (UTC). The starting point only decides who gets the small download: laptops on any update published from it on get a
**delta** (only the changed files), laptops on an older one or on none get the **full** bundle. The app picks by itself. `start` takes
a date (the first update published that day or later), an update number, `last` (the newest only) or `all`. The delta grows with
each update it covers, so move the start forward from time to time (`tools/release.sh start last`; the plan reminds you past 8).
Example: start 2026-11-18, updates published on 11-18 and 11-22; a laptop that installed the 11-22 update gets Friday's release as a
delta of what changed since 11-22.

The checks, in order:
0. **Tools:** git, curl, openssl, python3, a JDK, busybox (`tools/fetch_busybox.sh`), and `gh` logged in for an upload.
1. **git:** on `main` (`BRANCH`), no uncommitted changes in `usb/ lapis/ tools/ updater-app/`, `HEAD` = `origin/main`.
2. **Google build:** `lapis/BUILD` and `usb/patched/MANIFEST` name the same build (else `tools/port_build.sh --write`); the stick
   folder's `super.img` is that build (dump.erofs, when the `gbos-erofs` image exists).
3. **Key:** `usb/sysupdate.pub` is a real key, and the private key's public half.
4. **Tests:** `sh -n`, `test_system_update.sh`, `test_updater_core.sh`.
5. **Googlebook OS check** (what the app's Googlebook OS card needs before it offers "Download Rn and install"): reads the published
   feed. A newer Google build than published needs Google's `recovery.zip` still served and `super_parts.py` proving `super.img` from
   `lapis/parts` (which must be that build), so the feed gets its Google lines; otherwise it stops (`--allow-stick-only` publishes
   without them, laptops then use the stick). An older Google build than published is refused. `--check-google` also runs
   `tools/check_new_build.sh`.
6. Runs `publish_system_update.sh --since <start>` (it asks before uploading), then downloads the live feed and checks it is the one
   just signed.

## Publishing an update on GitHub (maintainer)

```sh
git push                                                   # the commit must be on GitHub (the app checks installed systems against it)
tools/publish_system_update.sh --key /media/offline-stick/gbos86-key/sysupdate-private.pem --tree /path/to/stick/gbos
```

It builds the set from the commit (`--commit REV`, default `HEAD`; `git archive`, so uncommitted edits never slip in), checks it the
way the laptop will, prints what it will upload and asks before uploading (`--yes` skips the question, `--no-upload` only makes the files,
in `build/releases/<seq>/`). It needs `gh`, the GitHub CLI, logged in once with `gh auth login`. On the repository (the `origin` remote, or
`--repo OWNER/NAME`) it creates the release `sysupdate-<seq>` with:

* `<name>-full.tar`: every file of the set **except `super.img`**. GitHub refuses files over 2 GiB, and `super.img` is Google's system
  image, which is not ours to hand out. A laptop that already has this Google build needs nothing else.
* `<name>-delta.tar`: only the files that differ from at least one of the last `--bases` published updates (default 3), so a laptop on
  any of them has everything else already. The app takes it when the laptop's set is one of those and unmodified.
* `<name>.manifest`: the set's manifest, used as a base by the next publish (it fetches the earlier ones with `gh`).
* `gbos86-update.feed`: what the app reads (below). The same file is then put on the release `sysupdate-feed`, which never moves, so
  the app always reads `https://github.com/OWNER/NAME/releases/download/sysupdate-feed/gbos86-update.feed`. The feed moves only after
  the new release is complete. Don't delete `sysupdate-feed`.

**A new Google build** (after porting it as in [porting.md](../porting.md)): publish the same way. Laptops on the older Google build need
the new `super.img`. They get it with the stick (menu 1), or, with the app's experimental setting, by rebuilding it from Google's own
download. For that the feed names Google's `recovery.zip` (`URL` in the commit's `lapis/BUILD`) and the size and sha256 of each partition
and of `super.img`. `tools/super_parts.py` first proves the published `super.img` can be rebuilt that way: it must be exactly
`make_super.py`'s image of Google's partitions in `lapis/parts` (`--parts DIR`). If not, the feed leaves the Google lines out and the app
asks for the stick.

## Building the app

`updater-app/build.sh` (plain Java, no Gradle or libraries) needs a JDK 17+ (`sudo apt install openjdk-17-jdk-headless`, about
125 MB) and the Android SDK's platform 35 and build-tools 35.0.0 in `~/Android/Sdk` (Google's command-line tools from
`dl.google.com/android/repository`, 157 MB, then `sdkmanager "platforms;android-35" "build-tools;35.0.0"`, about 130 MB more).
`updater-app/build.sh --test-key` signs with a throwaway key; a release build signs with `~/.config/gbos86/updater-app.p12`
(made on the first build; back it up, and Android only updates the app with the same key). The result is `usb/gbos86-updater.apk`;
on the stick it is in `install.sh`'s FILES, and `hook.sh` installs it with `pm` at boot when it changed. The APK is not committed.
The app's default update source is the project's main repository (`REPO` in `tools/release.conf`, where `tools/release.sh` publishes from `main`), whatever this clone's remotes are; a lab or fork build passes `REPO=OWNER/NAME updater-app/build.sh`. Settings can change it. Each APK carries a build stamp (repo, the git tree of `updater-app/`, `-dirty` for uncommitted changes, commit): `python3 -I tools/apk_stamp.py APK` prints it, and `tools/release.sh` refuses a tree whose APK points at another repository or was built from other `updater-app/` sources.

## The app

* **Material 3 interface.** Designed to Material Design 3 (Material You) guidelines: dynamic color tokens adapting to Day/Night mode and Android 12-15 system wallpaper accents; structured surface container cards (Hero Update status, System info & integrity, Local folder bundle, and Maintenance); M3 pill buttons (filled primary, tonal secondary, outlined, danger) with ripple effects; color-coded status badges for system integrity; segmented button controls for Settings; and a responsive layout (720dp max-width centered) suited for desktop and laptop screens.
* **Checking.** After each start, the gbOS86 boot service (`sysupdate.sh install-app`) tells the app (broadcast `BOOT_CHECK`; it reaches
  the app even if it was never opened). The app waits for a network, reads the feed, checks its signature against the laptop's own
  `gbos/sysupdate.pub`, and shows a notification when an update newer than the installed one is published. It also checks once a day,
  and when you open it. Checking never downloads the update.
* **Installing.** **Download and install** downloads the delta or the full bundle from GitHub (resumed after a broken connection; the
  sha256 must be the one in the signed feed), unpacks it in the app's own storage, and asks the root service to copy it into
  `update.inbox`. If the laptop lacks a file the delta does not carry (the root service says `NEEDS-FILES`), the app takes the full
  bundle by itself. The download runs as a foreground service with a notification, so it continues with the window closed; Stop
  stops it, and the next try resumes. When the update is staged, **Restart now** restarts the laptop.
* **Googlebook OS card.** Shows the running Google build by name (`ro.build.id`, e.g. `CL3B.260622.276.R4` → **Googlebook OS
  R4**). **Check for updates** reads the same feed: when the newest gbOS86 update brings a newer Google build (`google_build`, named by
  `google_version`), the card says so and offers **Download R5 and install**. That is the same install as the update card's (a new
  Google build only comes inside a signed gbOS86 update, with patches for it): first the **Large download** dialog (sizes from GitHub
  and Google, free space, time, metered-network warning; Continue/Cancel), then the usual confirmation. A newer build that cannot be
  downloaded (no Google lines, the setting is off, not enough space) is shown with the reason.
* **One restart.** After **Restart now** the update is installed during the start and confirmed about a minute after Android has
  started (`boot-ok`, below); meanwhile the app says **Finishing the update ...** and looks again every 15 s.
* **The modification check.** The app asks the root service for `sysupdate.sh audit`: every file of the installed set against
  `gbos/system.manifest` (changed, missing, and files added to the set's folders; `gbos.cfg` and logs never count; files over 64 MiB,
  i.e. `super.img`, by size unless you tap **Full check**). It also looks at the set's commit: `-dirty` means it was built from changed
  sources, and a clean commit is looked up on GitHub (`api.github.com/repos/OWNER/NAME/commits/<sha>`) to tell the project's published
  sources from a local or forked build. **Unless the system is exactly a published set**, the app shows an orange warning and asks
  again (**Install anyway** / **Copy anyway**) before installing. It warns when: the files differ from the record; the set was built
  from changed sources (`-dirty`); its commit is not in the project's sources on GitHub (unpushed or forked); the record names no
  commit; the laptop has no `system.manifest` or an unreadable one (installed from a stick with changes: `tools/deploy_stick.sh`
  deletes the stick's record when it copies changed files), so changes cannot be ruled out; or the check could not run. The warning
  says that the update replaces every system file with the published version, deletes files added to its folders, and was not tested
  with your changes. When the user goes ahead, the root service writes a line to `gbos/update/history.txt` (and the kernel log) with
  the reasons, e.g. `... after the app warned that this system is modified (norecord,unpublished); the user chose "Install anyway"`.
  The first update from the app installs every file and writes a `system.manifest`, so later checks are exact.
* **Settings**, on two tabs. **General:** check when the laptop starts (on), check once a day (on), delete old boot logs when GBOSSYS is
  short of space (off), "Download new Google builds from Google" (on; a size warning comes before each such download), the update
  source. **Experimental:** nothing at the moment.
* **Permissions:** internet, network state (the check waits for a network), notifications (granted by the boot service when it first
  installs the app; "Don't allow" is kept), start at boot, and a foreground service for the download.

### New Google builds straight from Google

Settings > General > Google builds > "Download new Google builds from Google" is **on by default** since 2026-11-23 (it was an
experimental opt-in; a user who turned it off keeps it off). Before anything starts, the app shows a "Large download" dialog: the
GitHub and Google download sizes, the free space needed on the data partition and on GBOSSYS, the time it takes, and a warning when the
network is metered ("Download anyway"). With the setting on, an update that brings a new Google build is installed without the stick.
The patches for the new build (usb/patched/, made with `tools/port_build.sh --write`) are part of the signed update itself. The app reads the directory of Google's
`recovery.zip` with two small range requests, then inflates the raw disk image inside it as one stream: the GPT and the `super` partition's
liblp metadata come from its first megabytes, the bytes of the six partitions go straight to their place in `super.img` (a Java port of
`make_super.py`, so the result is byte for byte the published one), and reading stops after the last of them. The zip is never stored. Then
every partition and the whole `super.img` must have the feed's sha256, and the root service copies it into the bundle on GBOSSYS, where
the next start checks it again like any other file.

* **Cost:** Google's file is read from the start up to the last partition: several GB (the whole zip is about 7.7 GB). It needs about
  4 GB free on the data partition while it works, and about 3.8 GB on GBOSSYS; the app checks GBOSSYS's space first.
* **Interruptions:** a broken connection is resumed in place (the same inflater carries on from the byte where it stopped, with
  `If-Range` so a changed file is noticed). If the app is closed or the laptop sleeps, Google's part starts over. Android 15 stops a
  download service after 6 hours a day.
* **Why it is safe to try:** nothing from Google is trusted. Only data with the hashes the maintainer signed is used, and the boot check
  is the same as for the stick's files.
* **Not tried yet on hardware or with Google's real file.** `tools/test_updater_core.sh` runs it against a small Google-like zip
  (zip64, a split partition placed out of order, a zero extent, a dropped connection). Once, by hand, the real partitions of build
  16693588 in a 3.2 GB zip64 test zip gave exactly the published `super.img` (sha256 `e94590cf...`, 29 s on the PC).

## How it works

### The bundle

```
gbos86-update-202611201230-3f0a1c2b9d4e/
  manifest          the whole set: header + one line per folder and file + the kernel
  manifest.sig      "rsa3072-sha256 <768 hex digits>"
  files/...         only the files that differ from the base (all of them in a full bundle); files/EFI/BOOTX64.EFI = the kernel
```

```
gbos86-system-set 1
seq 202611201230
commit 0123...(40 hex)[-dirty]
google_build 16693588
google_fingerprint Android/desktop_system/desktop:17/CL3B.260622.276.R5/16693588:user/release-keys
created 2026-11-20T12:30:00Z
d firmware                          folders, sorted (C order), parents first
f <sha256> <size> super.img          files, sorted
k <sha256> <size> EFI/BOOTX64.EFI    the kernel
end <number of d/f/k lines>
```

The set id is the manifest's sha256. Paths may only use `A-Z a-z 0-9 . _ + , @ = ~ - /` and may not hold `.` or `..` parts, a part
ending in a dot (FAT drops it), Windows device names, or two names that differ only in case (FAT does not tell them apart). The
first part may never be `gbos.cfg`, `logs`, `update*`, `.stamps`, `metadata.img`, `busybox`, `system.manifest` or `EFI`. Every file's
folder must be declared. The set must contain `super.img`, `super.layout`, `hook.sh`, `early_patch.sh`, `sysupdate.sh` and `sysupdate.pub`.
A bundle may hold nothing but `manifest`, `manifest.sig` and listed files under `files/`. A link, special file or unlisted file rejects it.

### The update feed

```
gbos86-update-feed 1
seq 202611201230
set <sha256 of the manifest>
name gbos86-update-000202611201230-<first 12 of set>
commit <40 hex>
google_build 16693588
google_version CL3B.260622.276.R5                (optional: lapis/BUILD's VERSION; the app shows "Googlebook OS R5")
created 2026-11-20T12:30:00Z
base <set id>                                   one per published set the delta covers
url https://github.com/OWNER/NAME/releases/download/sysupdate-202611201230/
asset full <file> <size> <sha256>
asset delta <file> <size> <sha256>              (optional)
asset manifest <file> <size> <sha256>
google_zip https://dl.google.com/device/recovery/lapis-user/16693588/recovery.zip      (optional, with the next lines)
google_part system_a <bytes, 4 KiB aligned> <sha256>                                    one per partition, in super.img's order
google_super <bytes> <sha256>
note <one line of what changed>                 (up to 20)
end <number of lines before this one>
rsa3072-sha256 <signature of everything above>
```

`google_version` is new (2026-11-24): an app older than that refuses a feed that has it ("unknown line"), so a laptop with an older
app gets that update through `update.inbox` or the stick once; the update brings the new app.

The app refuses a feed with a wrong signature, an unknown or malformed line, or a non-https address (Google's only on `dl.google.com`).
A replayed old feed can only hide a newer update; it cannot make the laptop take an older set (the boot check refuses a lower `seq`).

### The signature check, in busybox

The laptop has no openssl, so `sysupdate.sh` checks RSA PKCS#1 v1.5 / SHA-256 with busybox `bc`: s^65537 mod n by square-and-multiply
(about 0.4 s), after refusing a signature that is not smaller than n. It then rebuilds the whole expected block
(`00 01 FF..FF 00` + the SHA-256 DigestInfo + the manifest's hash) and compares it as one string, so no part of the padding is parsed or
skipped. The check runs on private copies of `manifest` and `manifest.sig`, so the files cannot change between the check and the use.

### At the next start (stage0)

`gbos.cfg` sources `early_patch.sh` right after the log folder exists, before `hwdetect` loads modules and before `super.img` is mounted.
`gbos_sysupdate_stage0` runs there, only on the installed system (`FROM=nvme`), only in stage0 itself (PID 1), once. It starts the
engine as a separate process with a clean environment (`env -i`), so nothing comes in from the kernel command line and nothing can change
stage0's variables. Kernels built after 2026-11-20 also have a 4-line fallback in `stage0_init.sh` for a `gbos.cfg` that does not source `early_patch.sh`.

1. A pending update is handled first (below). Otherwise the newest `gbos86-update-*` in `update.inbox` is moved to `update.staging`.
   Older ones are rejected, and `*.part` folders (an interrupted copy) are deleted.
2. Signature, strict manifest, bundle contents, the hash of every delivered file.
3. The set must be newer (`seq`) than the installed one (`gbos/system.manifest`), and its Google build must not be older.
4. Every listed file that is not in the bundle must already be installed with the listed hash (a delta made for another base is refused).
5. `sh -n` on every script of the new set; `early_patch.sh` must still call the updater; `sysupdate.pub` must hold a key.
6. `super.layout` must fit `super.img` (at most 6 partitions, `system_a` first). The new `super.img`'s `system/build.prop`, read
   through a read-only erofs loop mount on loop15, must have the manifest's fingerprint.
7. The kernel: if it differs from the installed one, it is written beside it as `BOOTX64.NEW` on the EFI partition and checked.
   Only `EFI/gbos86/BOOTX64.EFI` (dual-boot) or `EFI/BOOT/BOOTX64.EFI` on our own `GBOS_EFI` partition is ever written.
8. Free space for the switch is checked. Then a journal (`update/plan`) is written: `R` replace, `A` add, `D` delete, `K` kernel,
   `M` system.manifest. The engine copies itself to `update/engine.sh`, sets `phase=switching`, and runs the journal:
   * small files (≤ 1 MiB) are copied to `update.prev/`, then the new one is renamed over the old, so a script is never missing;
   * big files are renamed to `update.prev/`, then the new one renamed in;
   * the kernel is saved to `update/kernel.prev` on GBOSSYS, then `BOOTX64.NEW` is renamed over `BOOTX64.EFI`. On FAT that rewrites
     one directory entry, so the firmware always finds a whole old or a whole new kernel;
   * files of the set's own folders that the new set does not list go to `update.prev/`; `.stamps` is deleted.
9. `phase=trying`. If the kernel changed, the laptop restarts at once (the new kernel must not run with the old modules); otherwise
   this start continues with the new set and counts as try 1.

Any refusal leaves everything as it was and boots the current system.

### Power cuts, tries, commit and rollback

* **Power cut while switching:** the next start finds `phase=switching` (or a journal without a state file) and undoes the journal,
  newest step first. Each undo step only acts if its forward step happened, so undoing twice is harmless. The bundle is rejected
  ("copy it again"). While an update is pending, `early_patch.sh` runs `update/engine.sh`, the code that wrote the journal, rather than
  the new set's `sysupdate.sh`. If the engine stops with an unexpected error during a switch, the hook runs it once more to undo.
* **Did it work? (one restart)** While trying, `hook.sh` adds a service that, a minute after `sys.boot_completed`, writes the set id to
  `update/boot-ok` and **commits right away** (through `/dev/gbos-rw/stick`), then tells the app. So an update takes one restart: the
  user restarts once and the app says the system is up to date. A tried start that never reaches that point writes nothing, so the
  next start counts a failed try and rolls back after `SYSUPDATE_ATTEMPTS`, as before. If the commit is cut short, the next start
  finishes it: stage0 commits when `phase=committing`, when `boot-ok` holds the set id, or when a tried start's `kmsg.log` has
  `init: processing action (sys.boot_completed=1)` (not read when this start found the volume not cleanly unmounted). **Commit**
  deletes `update.prev`, the kernel copy and the journal. After a commit there is no rollback. The trade-off (as with Android's own
  A/B updates): a set that starts but breaks later in the session stays installed; the stick's menu 1 restores a working system.
* **Cleaning up the laptop's storage:** when the app stages a bundle, the root service records its folder in
  `/data/misc/gbos86/staged-source` (root only). After that set is committed, the app's cleanup job (after unlock, no
  network needed) deletes its own `files/updates/` and asks the root service (`cleanup`) to delete the recorded folder, for example the
  `gbos86-update-...` folder copied to Downloads. The root service deletes it only when the installed `system.manifest` is that set,
  no update is waiting or being tried, the folder is a `gbos86-update-*` folder under the recorded base, and its manifest is unchanged.
  After a rollback the folder is kept.
* **Rollback** after `SYSUPDATE_ATTEMPTS` tries without success: the journal is undone, the old kernel is written back the same
  safe way and the laptop restarts. With a new Google build that Android already ran (`hook.sh` writes `update/android-started`), it
  is held instead (see above) until the user asks with `update/rollback-request`.

### The stick's menu 1

Menu 1 shows the stick's and the laptop's sets (number, Google build, commit) and refuses an older Google build over a newer one unless
you type `DOWNGRADE`. Its "unchanged, skipped" shortcut (`.stamps`) is trusted only while the laptop's `system.manifest` is the one the
stamps were saved with, so an update made without the stick always gets a full compare. It never skips `super.img` when `super.layout`,
`libkm_compat.so`, `system_ext_manifest.xml` or `patched/MANIFEST` differ (they belong to one Google build). Afterwards it drops a pending
update's state, backups and kernel copy, and copies the stick's `system.manifest` only if it matches the stick's files.

## Security

* **What the signature protects:** only a set made and signed by the key holder is ever switched in, whole and unchanged, with
  scripts, patches and `super.img` from the same commit. That excludes corrupt or half-copied bundles, mismatched sets, bundles from
  anyone else, and replays of older sets (`seq`) or older Google builds.
* **What it cannot protect, today:** GBOSSYS is mounted `umask=000` and SELinux is permissive, so any Android app can already write
  `gbos/hook.sh`, which runs as root at the next start, and so also `sysupdate.sh` and `sysupdate.pub`. The updater does not add a way
  in. It is built so that it stays sound when that is fixed: the trust decision happens in stage0, with no Android process running.
  Making `/postinstall` writable only by root, and removing `gbos_sync`'s `run.sh` hook (which runs a file from GBOSSYS as root
  every 5 s), are separate tasks.
* **GitHub and Google are not trusted.** The app installs nothing that is not covered by the signature: the feed is signed, each
  download must have the feed's sha256, a rebuilt `super.img` the feed's hashes, and the laptop checks the signed manifest and every file
  again at boot. Someone who controls the GitHub account, the network or Google's server can at most stop updates.
* **The app is not trusted with anything.** It never installs. It writes a request into its own
  private folder (`/data/user/<user>/org.gbos86.updater/files/request.txt`, writable only by the app and root) and sets
  `sys.gbos86.sysupdate=<user>.<number>`. That property starts `gbos_sysupdate_req` (init, `seclabel u:r:shell:s0`, root), which reads
  the request (only if the app's own uid owns it), deletes it and answers in `response.txt`. Another app that sets the property finds
  no request to run. The service copies only from that user's shared storage (`/data/media/<user>/`) or the app's own `files/`
  folder (its downloads), only the files a signed manifest lists, and checks the signature before copying anything. Each source file is opened once and must be the file at that path (no link
  on the way, one hard link), and is hashed after the copy. So it cannot be used to copy someone else's private files onto GBOSSYS. A
  `.tar` is unpacked onto FAT, which cannot hold links.
* **With SELinux enforcing** (not today), this would need: a `gbos_sysupdate` domain for the service instead of `shell`, allowed to read
  `media_rw_data_file` and the app's data, write GBOSSYS (`vfat`) and run busybox from it; a `property_contexts` entry for
  `sys.gbos86.sysupdate` with a type that only the app's domain may set (the app signed with our certificate, given its own domain
  through `mac_permissions.xml` / `seapp_contexts`); and `hook.sh`'s own services moved out of `shell`. Until then, any app may set
  the property, which is harmless as described above.
* **Not repeated from a third-party updater that was reviewed and rated unsafe:** there is no root daemon taking requests from shared
  storage, no secret on shared storage, no `cp -r` of whole folders (only listed, hashed files reach GBOSSYS), no updater that updates
  itself from shared storage (a new `sysupdate.sh` arrives only inside a signed set and only takes over after the switch), no code
  fetched from a moving branch, no unverified multi-GB download, no block delta that cannot fit 8 GiB, no `super.img` without its
  `super.layout`, patches and scripts from the same commit, and no edits to `gbos.cfg`, other apps' settings or logs.

* **Writing GBOSSYS from Android.** Since the FAT lockdown (hook.sh `gbos_postinstall_lockdown`), Android's `/postinstall` is
  read-only. The three Android-side writers (`stage`, `cancel`, `boot-ok`) write through `/dev/gbos-rw/stick`: stage0's `/gbos`
  read-write (nosuid, nodev, noexec) inside `/dev/gbos-rw`, a 0700 root directory, so no app and not system_server can reach it
  (hook.sh `gbos_root_rw`). With an older hook.sh they re-run in a mount namespace of their own with `/postinstall` remounted
  read-write. Before writing they really write a probe directory (busybox `test -w` is always true for root). When the target is still
  read-only they refuse and say why, also when the kernel made the FAT itself read-only after a FAT error (then nothing can write it
  until the FAT is checked). Each run logs the mount lines and the way it chose to `logs/boot-N/sysupdate.txt`, and the app's install
  service logs the same mount lines to `logs/boot-N/updater-app.txt` every boot. Tested with real mounts in a privileged container
  (`tools/test_postinstall_ro.sh`), running `sysupdate.sh` exactly as init does.

## Files on GBOSSYS

| Path | What |
|---|---|
| `gbos/system.manifest` | the installed set (copied from the bundle; menu 1 copies the stick's) |
| `gbos/update.inbox/` | bundles waiting for the next start (yours to copy into) |
| `gbos/update.staging/` | the bundle being installed or tried |
| `gbos/update.prev/` | the replaced files, until commit or rollback |
| `gbos/update.rejected/<name>/` | manifest + `reason.txt` of the last 5 refused or rolled-back bundles |
| `gbos/update/state`, `plan`, `engine.sh`, `kernel.prev` | the pending update (never edit them) |
| `gbos/update/status.txt`, `history.txt` | the last result, and the last 200 |
| `gbos/update/boot-ok`, `android-started`, `rollback-request` | markers (the last one is yours to create) |

## Limits and open points

* A kernel that never reaches stage0 (a panic before it) cannot roll itself back: the old kernel is in `gbos/update/kernel.prev`,
  but only stage0 can put it back. Boot the stick and use menu 1. A UEFI `BootNext` trial of the new kernel would avoid this, but
  needs efivarfs and `efibootmgr` on every laptop. Kernels are tested in QEMU (`kernel/isa_emu_test/run_qemu.py`) before they ship.
* The messages go to `tty1`. A kernel that unbinds fbcon for a quiet console would hide them; the logs (`kmsg.log`, `status.txt`) still
  have them.
* Rolling back a new Google build after Android has run it needs a way to undo `/data` (Android's userdata checkpoint with dm-bow),
  which gbOS86 does not have.
* A top-level folder that a new set no longer mentions is left on GBOSSYS (that is what keeps `camera/`). To really remove one, use menu 1.
* Not yet tried on the laptop: every case above runs in `tools/test_system_update.sh` (116 checks) against a fake GBOSSYS and EFI
  folder with the stick's busybox, and the app's core in `tools/test_updater_core.sh`. The loop mounts, the EFI partition lookup, the
  restart, the app's screens and services, GitHub's redirects and Google's real `recovery.zip` only run on hardware.
* The automatic check needs the gbOS86 boot service (`gbos86-updater.apk` on GBOSSYS) or the app opened once since it was installed.

## Changes from the first design, and why

* **The kernel is replaced by one rename over the old file**, not "rename the old one away, rename the new one in": between those two
  renames the laptop would have had no kernel to boot.
* **The kernel backup lives on GBOSSYS** (`update/kernel.prev`), not as `BOOTX64.PRV` on the EFI partition. A Windows EFI partition
  is often 100 MB and may not hold two extra 34 MB kernels.
* **Small files are copied to the backup, then replaced by a rename over them**, so `early_patch.sh` and `sysupdate.sh`, which do the
  recovery, are never missing after a power cut.
* **The code that undoes a switch is the code that started it** (`update/engine.sh`), not a possibly broken new `sysupdate.sh`.
* **The engine runs as its own process with `env -i`**, not sourced into stage0. It may exit freely, and the kernel command line
  cannot inject settings.
* **The app is installed like the KernelSU manager** (`pm install` at boot), not bound in as a privileged app. Privileged status gives
  nothing while SELinux is permissive and the app is not trusted anyway, and overlaying Google's read-only `priv-app` folders is fragile.
* **The root service copies through checked file descriptors and checks the signature first**, so it cannot be turned into a way to
  copy private files to the world-readable GBOSSYS.
* **Updates come from GitHub releases, without `super.img`** (2026-11-20). The app downloads them itself, so no second computer is
  needed. `super.img` cannot go on GitHub (2 GiB per file, and it is Google's), so a new Google build comes from the stick or, as an
  experimental setting, is rebuilt from Google's own download and checked against the signed hashes.
* **The feed carries its signature as its last line**, so replacing it on GitHub is one upload and a reader never pairs a new feed with
  an old signature.
* **Logs are deleted only when the user asks** (`--prune-logs`, the app's tick box), answering the open question about space.
* **`camera/` is not part of a set by default**, and a set never removes a top-level folder it does not mention.
