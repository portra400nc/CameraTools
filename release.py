#!/usr/bin/env python3
"""Publish a CameraTools release on GitHub.

Zips CameraTools' built files into "Camera Tools X.Y.Z/", tags the version from
AssemblyInfo.cs, and creates the GitHub release with the zip and notes from git-cliff.

    python3 release.py                 # show the notes, ask, then publish
    python3 release.py --zip-only      # only write release/Camera-Tools-X.Y.Z.zip
    python3 release.py --from <folder> # take the files from anywhere under <folder>

Without --from, the files come from the build outputs in this repository.
Needs git-cliff and gh.
"""
import argparse
import re
import subprocess
import sys
import zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent
BUILT = [
    REPO / "CameraTools/bin/Release/CameraTools.dll",
    REPO / "CameraTools/bin/Release/CameraTools.pdb",
    REPO / "ReShadeBridge/bin/CameraToolsReShadeBridge.addon64",
    REPO / "Enhancements/bin/CameraToolsEnhancements.addon64",
]


def run(*args, capture=False):
    result = subprocess.run(args, cwd=REPO, check=True, text=True, capture_output=capture)
    return result.stdout.strip() if capture else None


def fail(message):
    sys.exit(f"release: {message}")


def main():
    parser = argparse.ArgumentParser(description="Publish a CameraTools release on GitHub.")
    parser.add_argument("--zip-only", action="store_true", help="only write the zip")
    parser.add_argument("--from", dest="source", type=Path, help="a folder to find the files in")
    args = parser.parse_args()

    info = (REPO / "CameraTools/Properties/AssemblyInfo.cs").read_text()
    version = re.search(r'MelonInfo\(.*"Camera Tools", "([^"]+)"', info).group(1)
    tag = f"v{version}"

    files = BUILT
    if args.source:
        files = [next(args.source.rglob(f.name), f) for f in BUILT]
    missing = [str(f) for f in files if not f.is_file()]
    if missing:
        fail("missing " + ", ".join(missing))
    # The DLL's file version is stored as UTF-16, as in "1.2.1.0".
    if f"{version}.0".encode("utf-16-le") not in files[0].read_bytes():
        fail(f"{files[0]} is not version {version}. Build it first.")

    out = REPO / "release" / f"Camera-Tools-{version}.zip"
    out.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            z.write(f, f"Camera Tools {version}/{f.name}")
    print(f"Wrote {out}")
    if args.zip_only:
        return

    if run("git", "status", "--porcelain", "--untracked-files=no", capture=True):
        fail("commit your changes first")
    if run("git", "tag", "--list", tag, capture=True):
        fail(f"{tag} already exists. Raise the version in AssemblyInfo.cs.")
    run("git", "fetch", "-q", "origin")
    if subprocess.run(["git", "merge-base", "--is-ancestor", "HEAD", "origin/master"], cwd=REPO).returncode:
        fail("push master first")

    notes = run("git", "cliff", "--unreleased", "--tag", tag, "--strip", "all", capture=True)
    print(f"\n{notes}\n")
    if input(f"Publish Camera Tools {tag}? [y/N] ").strip().lower() != "y":
        return

    run("git", "tag", tag)
    run("git", "push", "origin", tag)
    notes_file = out.with_suffix(".md")
    notes_file.write_text(notes + "\n")
    run("gh", "release", "create", tag, str(out), "--title", f"Camera Tools {tag}", "--notes-file", str(notes_file))


if __name__ == "__main__":
    main()
