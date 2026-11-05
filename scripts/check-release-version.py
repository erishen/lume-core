#!/usr/bin/env python3
"""Gate every release-version literal against the Makefile's declaration.

Makefile is the single source for `lume --version` / `lume-core --version`.
Two other places repeat the number by hand:

  CHANGELOG.md        the newest `## [x.y.z]` heading
  Dockerfile.scanner  ARG LUME_VERSION=vX.Y.Z, which decides WHICH release
                      tarball the scanner image downloads (host repo only)

release.yml asserts the tag against the Makefile only, so a stale copy used
to be undetectable: bump the Makefile, forget Dockerfile.scanner, and the
image silently ships the previous release's binary.  Run this on every push
and PR, and pass --tag on tag events to close the loop.

    python3 scripts/check-release-version.py
    python3 scripts/check-release-version.py --tag v0.12.2
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

MAKE_VERSION = re.compile(
    r"^(?:LUME_VERSION|LUME_CORE_VERSION)\s*\?=\s*(\S+)", re.M)
# Skip `## [Unreleased]` and other prose headings: only a heading starting
# with a digit is a released version (core's CHANGELOG has an Unreleased
# section above the newest 0.13.0 entry).
CHANGE_VERSION = re.compile(r"^##\s+\[([0-9][^\]]*)\]", re.M)
SCANNER_VERSION = re.compile(r"^ARG\s+LUME_VERSION=v?(\S+)", re.M)


def read(path):
    if not os.path.isfile(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def unv(value):
    return value[1:] if value.startswith("v") else value


def main():
    tag = None
    if "--tag" in sys.argv:
        i = sys.argv.index("--tag")
        if i + 1 >= len(sys.argv):
            print("FAIL  --tag needs a value (e.g. --tag v0.12.2)")
            return 1
        tag = unv(sys.argv[i + 1])

    patterns = {"Makefile": MAKE_VERSION,
                "CHANGELOG.md": CHANGE_VERSION,
                "Dockerfile.scanner": SCANNER_VERSION}

    found = {}
    for name in ("Makefile", "CHANGELOG.md", "Dockerfile.scanner"):
        text = read(os.path.join(ROOT, name))
        if text is None:
            # Dockerfile.scanner exists only in the host repo; core has none.
            print("skip  %-20s (not in this repo)" % name)
            continue
        m = patterns[name].search(text)
        if not m:
            print("FAIL  %-20s no version literal found" % name)
            return 1
        found[name] = unv(m.group(1))
        print("      %-20s %s" % (name, found[name]))

    declared = found.get("Makefile")
    if not declared:
        print("FAIL  Makefile has no LUME_VERSION / LUME_CORE_VERSION")
        return 1

    bad = [n for n, v in found.items() if v != declared]
    for n in bad:
        print("FAIL  %-20s %s != Makefile %s" % (n, found[n], declared))
    tag_bad = tag is not None and tag != declared
    if tag_bad:
        print("FAIL  release tag %s != Makefile %s" % (tag, declared))
    if bad or tag_bad:
        print("release version literals disagree; bump them together")
        return 1
    print("ok    release version %s (%d literal(s), tag %s)"
          % (declared, len(found), tag if tag is not None else "not checked"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
