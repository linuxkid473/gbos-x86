---
name: gbos-release
description: Use when the maintainer wants to release / publish a gbOS86 system update (git changes or a new Googlebook OS / Google build) from the main branch to GitHub releases for the Updater app.
---

# Releasing a gbOS86 update

Everything goes through `tools/release.sh` (details: `docs/system-update.md`, "Releasing from main"). It checks first, publishes last.

Only for the main repository (`REPO` in `tools/release.conf`, linuxkid473/gbos-x86). Never for the lab repository or a fork:
the script refuses a clone whose origin is not the main repository; don't work around it.

0. **Once per computer:** `tools/release.sh setup` (the maintainer types the key and stick-folder paths).
1. **Plan:** `tools/release.sh` shows what a release made today contains and which laptops get a small download. The starting
   point is `START` in `tools/release.conf`; change it only when the maintainer asks: `tools/release.sh start DATE|SEQ|last|all`,
   then commit and push `tools/release.conf`.
2. **Check:** `tools/release.sh check`. Fix every `FAIL`. Common ones:
   - not on `main` / uncommitted / not pushed: commit and push (ask the maintainer before pushing).
   - `patched/MANIFEST is for ...`: the Google build changed; port it (`porting.md`: `tools/port_build.sh --isa`, then `--write --product`).
   - `tree's super.img is ...`: the stick folder holds another Google build's `super.img`.
   - `laptops could not download Google build ...`: Google no longer serves the `recovery.zip`, or `lapis/parts` is not this build.
     Ask the maintainer before using `--allow-stick-only` (laptops then need the stick for the new Google build).
   - `no busybox`: `tools/fetch_busybox.sh`. A test failing: debug it; never `--skip-tests` for a real release.
3. **Dry run** (optional): `tools/release.sh publish --dry-run`. Files land in `build/releases/<seq>/`.
4. **Publish:** `tools/release.sh publish`. The maintainer runs this themselves (or approves it): the private key is on their
   offline stick, it asks for the passphrase, and it asks `Type yes` before uploading. Don't pass `--yes` unless they ask.
5. After `released:` add a dated line to `HANDOFF.md` (seq, set id, Google build, commit) and commit it.

Never commit the private key, Google images (`lapis/parts`, `*.img`) or tokens. New Google builds: `tools/check_new_build.sh`
(or `tools/release.sh check --check-google`).
