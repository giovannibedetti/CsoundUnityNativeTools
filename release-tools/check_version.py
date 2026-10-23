#!/usr/bin/env python3
"""The package version lives in two files that have to agree.

    check_version.py --package <dir>                 report both, exit 1 if they differ
    check_version.py --package <dir> 4.0.0-alpha.1   write that version into both

The package is passed in rather than deduced, because this script lives in the
maintainer repository and not inside the package it writes to — which is what lets
CI run it.

The Doxyfile is not one of them: it takes the number through DOXYGEN_PROJECT_NUMBER
when the documentation is generated, so there is one fewer place to disagree.
"""
import io
import os
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def paths_from_argv(argv):
    """Returns (package dir, version or None) and leaves argv clean."""
    pkg = os.environ.get("PACKAGE_DIR")
    rest = []
    it = iter(argv)
    for a in it:
        if a == "--package":
            pkg = next(it, None)
        else:
            rest.append(a)
    if not pkg:
        sys.exit("  --package <dir> is required (or set PACKAGE_DIR)")
    return Path(pkg).resolve(), rest[0] if rest else None


PKG, WANTED = paths_from_argv(sys.argv[1:])

# (label, path, regex with one capturing group around the version)
PLACES = [
    ("package.json",     PKG / "package.json",
     re.compile(r'("version"\s*:\s*")([^"]+)(")')),
    ("CsoundUnity.cs",   PKG / "Runtime" / "CsoundUnity.cs",
     re.compile(r'(packageVersion\s*=\s*")([^"]+)(")')),
]


def read(path, rx):
    if not path.exists():
        return None, f"missing: {path}"
    text = io.open(path, encoding="utf-8", newline="").read()
    hits = rx.findall(text)
    if len(hits) != 1:
        return None, f"{len(hits)} matches in {path.name}, expected 1"
    return hits[0][1], None


def main():
    wanted = WANTED
    found, problems = {}, []

    for label, path, rx in PLACES:
        value, err = read(path, rx)
        found[label] = value
        if err:
            problems.append(err)

    width = max(len(l) for l, _, _ in PLACES)
    for label, path, _ in PLACES:
        print(f"  {label:<{width}}  {found[label] or '—'}    {path}")

    if problems:
        for p in problems:
            print(f"\n  {p}")
        return 1

    if wanted is None:
        values = set(found.values())
        if len(values) == 1:
            print(f"\nboth agree: {values.pop()}")
            return 0
        print("\nTHEY DIFFER — a release built from this tree would carry the wrong version")
        return 1

    for label, path, rx in PLACES:
        text = io.open(path, encoding="utf-8", newline="").read()
        io.open(path, "w", encoding="utf-8", newline="").write(
            rx.sub(lambda m: m.group(1) + wanted + m.group(3), text, count=1))
    print(f"\nwritten: {wanted}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
