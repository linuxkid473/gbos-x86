#!/usr/bin/env python3
"""apk_stamp.py APK: print the gbOS86 Updater's build stamp (updater-app/build.sh BuildInfo.BUILD_STAMP) as
"repo=OWNER/NAME tree=<git tree of updater-app>[-dirty] commit=<sha>". Exit 1 when the APK has no stamp
(built before the stamp existed), 2 when it is not a readable APK or holds more than one stamp."""
import re
import sys
import zipfile

STAMP = re.compile(rb'gbos86-build repo=([A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+|-) tree=([0-9a-f]{40}(?:-dirty)?|unknown(?:-dirty)?) '
                   rb'commit=([0-9a-f]{40}|unknown)')


def main():
    if len(sys.argv) != 2:
        print("usage: apk_stamp.py APK", file=sys.stderr)
        return 2
    try:
        with zipfile.ZipFile(sys.argv[1]) as z:
            found = set()
            for n in z.namelist():
                if re.fullmatch(r'classes\d*\.dex', n):
                    found |= {m.groups() for m in STAMP.finditer(z.read(n))}
    except (OSError, zipfile.BadZipFile) as e:
        print(f"apk_stamp: {sys.argv[1]}: {e}", file=sys.stderr)
        return 2
    if not found:
        print(f"apk_stamp: {sys.argv[1]}: no build stamp (built before updater-app/build.sh stamped its APKs)", file=sys.stderr)
        return 1
    if len(found) > 1:
        print(f"apk_stamp: {sys.argv[1]}: {len(found)} different build stamps", file=sys.stderr)
        return 2
    repo, tree, commit = (x.decode() for x in found.pop())
    print(f"repo={repo} tree={tree} commit={commit}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
