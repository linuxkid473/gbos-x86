# Bug report card (built 2026-11-26)

**Decided (user, 2026-11-26):** the report is encrypted for the maintainer and attached to a GitHub issue on the update source repo.
Built as (Share button added 2026-11-26: the system share sheet with the same encrypted .zip): `sysupdate.sh report` (root: collect, redact, REPORT.md, .tar.gz), `core/ReportCrypto.java` (RSA-OAEP + AES-CTR + HMAC),
`Report.java` (saves `Download/gbOS86/gbos86-report-ID-TIME.zip`), the card in `MainActivity`, the key file `usb/report.pub`
(placeholder until the maintainer runs `tools/open_report.sh keygen`), and `tools/open_report.sh open` for the maintainer. The issue
page is pre-filled with the report id, the update number and the Googlebook OS version only; the user's text is inside the encrypted file.

**Maintainer, reading a report:** download the .zip from the issue, then
`tools/open_report.sh open gbos86-report-....zip --key ~/path/report-private.pem --out /tmp/reports` and read the REPORT.md it prints.

The investigation that led to it follows.

Goal: a sixth card in the Updater app, **Bug report**. The user describes the problem in a text field and taps **Generate report**.
The app writes one file holding the description, this laptop's ID and specs, and the latest logs, then offers to send it.
The maintainer reads it with Claude Code, so the file is laid out for an agent: one entry point, fixed names, the same folder names as on the stick.

## What exists already (checked)

* **Logs are root-only.** Under the FAT lockdown `/postinstall` is read-only for Android and `logs/boot-N` is writable by root only
  (`hook.sh` "POSTINSTALL: ... logs/boot-16 writable by root only"). The app cannot read them itself.
* **The root service already does this kind of job.** `sysupdate.sh request` (init service `gbos_sysupdate_req`, started by the
  property `sys.gbos86.sysupdate`) reads one request file from the app's private folder, checks its owner, runs an action
  (`status`, `info`, `audit`, `cancel`, `stage`, `cleanup`) and writes the answer back into that folder, `chown`ed to the app's uid
  (`respond()`). A new action `report` fits this exactly. `Root.call()` in the app is the client.
* **One boot's logs** (boot-21, a full session): about 9.8 MB, **1.3 MB as tar.gz**. Two boots (the current one so far, plus the
  previous complete one) are about 2.5 MB: fine for e-mail, chat apps and GitHub attachments (25 MB limit).
* **Specs are mostly in the logs already:** `kmsg.log` has `DMI: Dell Inc. Latitude 7320 Detachable/0TNW0H, BIOS 1.49.0` and
  `Linux version ... #N`; `hw.txt` has the CPU, memory, PCI list, block devices and DRM; `hwdetect.txt` has the detection result;
  `stage0-dmesg.txt` has `PATCH-STATUS`, the lab marker and the update lines. `sysupdate.sh info` gives the set, seq, commit and Google build.
* **Personal data found in one boot's logs:** USB serial numbers (`SerialNumber: ...`, e.g. the stick's), 12 MAC addresses, Wi-Fi
  `SSID:` lines (empty in that boot, real names in others), `@` names that are not e-mails here but could be (accounts). No DMI
  serial / service tag in hw.txt or kmsg.

## Design

### Laptop ID (privacy)

Not the service tag or any serial. A random ID made once, `gbos86-<8 hex>`, kept in `gbos/report-id` on GBOSSYS (survives app
reinstalls; a reinstall from the stick makes a new one). It lets the maintainer match several reports from one laptop and nothing else.

### Flow

1. App: text field (max ~4000 chars) + checkbox "include the previous boot too" (on) + **Generate report**.
2. App writes `report.txt` (the description) in its private folder and calls `Root.call("report", ...)`.
3. Root service (`sysupdate.sh report`, as root):
   * copies `logs/boot-<current>` (as written so far) and `logs/boot-<previous>`, using `dd conv=noerror,sync` + strip NULs (FAT trap
     in AGENTS.md: a damaged file must not stop the report, and must not make the volume read-only: skip `kmsg.log` reads when the
     volume was mounted dirty, as `su_booted_ok` does);
   * adds `update/status.txt`, `update/history.txt` (tail 50), `sysupdate.sh info` and `audit` output, the first lines of
     `system.manifest` (set, seq, commit, google_build: no file list), `gbos.cfg`, `getprop` (filtered, below);
   * **redacts** with sed, replacing each value with a stable token so lines still correlate: MAC → `MAC-1..n`, `SerialNumber:` /
     `serialno` / `ro.serialno` → `SERIAL-n`, `SSID` values → `SSID-n`, e-mail addresses → `EMAIL-n`, account names in
     `Account {name=...}` → `ACCOUNT-n`;
   * writes `REPORT.md` (below) and `SHA256SUMS`, packs `gbos86-report-<id>-<UTC yyyymmddHHMM>.tar.gz` into the app's folder, `chown`s it.
4. App: copies it to **Downloads** (MediaStore, so the user can also find it in Files), then shows:
   * **Share**: Android share sheet with the file (Gmail, Drive, Discord, ...), subject and text = the short summary;
   * **Open GitHub issue**: `https://github.com/<update source repo>/issues/new?title=...&body=...` with the summary prefilled (under
     ~6 KB, URL limits) and "attach the .tar.gz from Downloads". No token in the app: GitHub's API cannot attach files to issues
     anyway, and a token in a public APK would be abused.

### `REPORT.md`, the entry point (for Claude Code)

```markdown
---
gbos86_report: 1
id: gbos86-3fa91c2e
created: 2026-11-26T14:03:00Z
laptop: Dell Inc. Latitude 7320 Detachable/0TNW0H, BIOS 1.49.0
cpu: 11th Gen Intel Core i7-1180G7
ram_kb: 15957640
kernel: "6.18.21-gbos-whl #64"
set: {seq: 202611241104, id: 8190da52..., commit: 6907824..., google_build: 16693588, google_version: CL3B.260622.276.R5}
update: {pending: none, last: "update committed: set seq ... works"}
modified: no            # sysupdate.sh audit
boots: [boot-23 (current, partial), boot-22 (previous, complete)]
redacted: [MAC, SERIAL, SSID, EMAIL, ACCOUNT]
---
## What the user says
<verbatim text>

## Signals found automatically (file:line)
- PATCH-STATUS 6 applied, 0 not applied   logs/boot-23/stage0-dmesg.txt:412
- FATAL EXCEPTION x2 (com.example)          logs/boot-23/logcat.txt.03:8812
- "Waited one second for" x0, ANR x1, crash.txt empty, FAT-CHECK: ok
## Files
logs/boot-N/  exactly as on the stick (names in docs and HANDOFF.md apply)
system/       info.txt, audit.txt, status.txt, history.txt, manifest-head.txt, gbos.cfg, getprop.txt
## Reading it
Start with "What the user says", then the signals, then the boot folders. Repo: AGENTS.md "Traps" applies
(FAT logs, PATCH-STATUS, Trusty waits, drmhwc on i915).
```

The signal list is a fixed set of greps the maintainer already uses in HANDOFF (PATCH-STATUS, FAT-CHECK, `FATAL EXCEPTION`, `ANR in`,
`Waited one second for`, `BOOT FAILURE`, `NOT patched`, `FAT-fs`, `I/O error`, `USB disconnect`, crash.txt size), each with its first
file:line, so an agent knows where to open first without reading 10 MB.

### Where the code goes

| Part | File | Size (estimate) |
|---|---|---|
| `report` action, collection, redaction, REPORT.md, tar.gz | `usb/sysupdate.sh` (`cmd_report`, a `report)` case in `cmd_request`) | ~120 lines of sh |
| report id | `usb/sysupdate.sh` (made on first use) | few lines |
| card, text field, buttons, share, MediaStore copy, issue URL | `updater-app/.../MainActivity.java` + a `Report.java` helper | ~200 lines |
| FileProvider for the share sheet | `AndroidManifest.xml` + `res/xml/file_paths.xml` | small |
| tests | `tools/test_system_update.sh` (redaction on fixture lines: no MAC/serial/SSID/e-mail left; damaged FAT file skipped; size cap), `tools/test_updater_core.sh` for the URL builder | |

No new permission: the share sheet and MediaStore Downloads need none on Android 10+.

## Choices (as decided)

1. **Where reports go:** a GitHub issue on the update source repo, the attachment encrypted for the maintainer (decided).
2. **How much log:** current + previous boot (~2.5 MB) (`--boots`, default 2).
3. **Redaction:** MAC, serial numbers, Wi-Fi names, account names, e-mail addresses (common TLDs only, so `name@com.android.x`
   package-style names stay readable).
4. **Channel:** first built on `latitude-7320-integration` and tested on the laptop as a lab update. On main the app's update source,
   and so the issue page, is `REPO` in `tools/release.conf` (the main repository, through `updater-app/build.sh`), and `usb/report.pub`
   stays the placeholder until the main maintainer runs `tools/open_report.sh keygen`. The lab's report key is never committed here.
