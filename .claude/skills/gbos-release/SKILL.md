---
name: gbos-release
description: Use when the maintainer wants to release / publish a gbOS86 system update (git changes or a new Googlebook OS / Google build) from the main branch to GitHub releases for the Updater app.
---

# Releasing a gbOS86 update

Everything goes through `tools/release.sh` (details: `docs/system-update.md`, "Releasing from main"). It checks first, publishes last.

1. **Check** (no key, nothing uploaded):
   ```sh
   tools/release.sh check --tree /path/to/stick/gbos
   ```
   Fix every `FAIL` it prints. Common ones:
   - not on `main` / uncommitted / not pushed: commit and push (ask the maintainer before pushing).
   - `patched/MANIFEST is for ...`: the Google build changed; port it (`porting.md`: `tools/port_build.sh --isa`, then `--write --product`).
   - `tree's super.img is ...`: the stick tree holds another Google build's `super.img`.
   - `laptops could not download Google build ...`: Google no longer serves the `recovery.zip`, or `lapis/parts` is not this build.
     Ask the maintainer before using `--allow-stick-only` (laptops then need the stick for the new Google build).
   - a test failing: debug it; never `--skip-tests` for a real release.
2. **Dry run** (optional): `tools/release.sh publish --key KEY.pem --tree DIR --dry-run`. Files land in `build/releases/<seq>/`.
3. **Publish:** `tools/release.sh publish --key KEY.pem --tree DIR`. The maintainer runs this themselves (or approves it): the private
   key is on their offline stick, it asks for the passphrase, and it asks `Type yes` before uploading to the public repository.
   Don't pass `--yes` unless they ask.
4. After `released:` add a dated line to `HANDOFF.md` (seq, set id, Google build, commit) and commit it.

Never commit the private key, Google images (`lapis/parts`, `*.img`) or tokens. New Google builds: `tools/check_new_build.sh`
(or `tools/release.sh check --check-google`).
