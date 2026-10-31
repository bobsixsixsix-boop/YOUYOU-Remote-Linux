"""Fetch the pinned, GPL-licensed client and its hash-verified dependencies."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

COMMIT = "ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b"
ARCHIVE_HASH = "d90ec380f10e5c7bf0c8f9a28f00f432f0bba254010e3a22855ad5b2cb72f9a8"


def prepare(target):
    marker = target / ".youyou-linux-upstream"
    if not target.exists():
        target.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=target.parent) as scratch:
            scratch = Path(scratch)
            archive = scratch / "project.tar.gz"
            url = f"https://codeload.github.com/doke69/YOUYOU-Remote-ns/tar.gz/{COMMIT}"
            with urllib.request.urlopen(url, timeout=120) as remote, archive.open("wb") as out:
                from shutil import copyfileobj
                copyfileobj(remote, out)
            if hashlib.sha256(archive.read_bytes()).hexdigest() != ARCHIVE_HASH:
                raise ValueError("Upstream archive failed SHA256 validation")
            with tarfile.open(archive) as tar:
                tar.extractall(scratch, filter="data")
            src = scratch / f"YOUYOU-Remote-ns-{COMMIT}"
            (src / marker.name).write_text(COMMIT)
            os.rename(src, target)
    if not marker.is_file() or marker.read_text() != COMMIT:
        raise ValueError(f"{target} is not a managed source directory, choose an empty path")
    for script in ("fetch_deps.py", "patch_deps.py"):
        subprocess.run([sys.executable, target / "scripts" / script], check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    prepare(parser.parse_args().output.resolve())
