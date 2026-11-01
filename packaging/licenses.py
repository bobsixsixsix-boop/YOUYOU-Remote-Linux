"""Write /usr/share/doc/youyou-linux/copyright: the notices of exactly what this build links.
Runs where the build ran (the Y700): needs core/upstream/external from upstream's
fetch_deps.py and the crates in Cargo's cache (cargo metadata --offline).
Usage: python3 packaging/licenses.py <output>"""
import json
from pathlib import Path
import subprocess
import sys
import tomllib

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "core/upstream"
# The core/upstream subtree (git log --grep git-subtree-split); bump with every subtree pull.
UPSTREAM_REF = "ba4d7f9f8f87f4a47e4d872db3dd0ca526718f8b"
TARGET = "aarch64-unknown-linux-gnu"
# What core/CMakeLists.txt links from upstream's external/ (qrcodegen is not).
LINKED = {"libdatachannel", "libjuice", "libsrtp", "mbedtls", "json", "usrsctp", "plog"}


def notices(folder):
    return [p for p in sorted(folder.iterdir())
            if p.is_file() and p.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))]


def collect():
    texts = {}  # shared texts (Apache-2.0 above all) once, with every package that uses them
    inventory = []

    def add(label, path):
        body = path.read_text(encoding="utf-8-sig").strip()
        if not body:
            raise ValueError(f"Empty notice for {label}")
        texts.setdefault(body, []).append(label)

    for path in ("LICENSE", "NOTICE.md"):
        add(f"YOUYOU Remote Linux, {path}", ROOT / path)
    for path in ("LICENSE", "NOTICE.md"):
        add(f"YOUYOU-Remote-ns (doke69, after Antman2023's UURemote-ns), {path}", UPSTREAM / path)

    inventory.append(f"Protocol core: https://github.com/doke69/YOUYOU-Remote-ns/tree/{UPSTREAM_REF}")
    for dep in json.loads((UPSTREAM / "scripts/sources.lock.json").read_text()):
        if dep["name"] not in LINKED:
            continue
        inventory.append(f'{dep["name"]}  https://github.com/{dep["repo"]}/tree/{dep["ref"]}')
        found = notices(UPSTREAM / "external" / dep["path"])
        if not found:
            raise ValueError(f'Missing native notices for {dep["name"]}')
        for path in found:
            add(f'{dep["name"]}, {path.name}', path)
    # This declaration is in HACL*'s source header, the Apache terms appear in Mbed TLS' LICENSE.
    inventory.append("Mbed TLS Everest/HACL*: Copyright (c) INRIA and Microsoft Corporation. All rights reserved. Apache-2.0.")

    meta = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--offline", "--locked", "--filter-platform", TARGET, "--format-version", "1"],
        cwd=ROOT / "app"))
    # Only what ends up in the binary: the normal-dependency closure of the app (no build tools).
    nodes = {n["id"]: n for n in meta["resolve"]["nodes"]}
    linked, todo = set(), [meta["resolve"]["root"]]
    while todo:
        node = todo.pop()
        if node not in linked:
            linked.add(node)
            todo += [d["pkg"] for d in nodes[node]["deps"] if any(k["kind"] is None for k in d["dep_kinds"])]
    locked = tomllib.loads((ROOT / "app/Cargo.lock").read_text())
    sums = {(p["name"], p["version"]): p["checksum"] for p in locked["package"] if "checksum" in p}
    crates = [p for p in meta["packages"] if p["id"] in linked and p.get("source")]
    for p in sorted(crates, key=lambda p: (p["name"], p["version"])):
        name, version = p["name"], p["version"]
        folder = Path(p["manifest_path"]).parent
        inventory.append(f'{name} {version}  {p["license"]}  https://crates.io/crates/{name}/{version}  SHA256 {sums[(name, version)]}')
        found = notices(folder)
        if name == "sdl2-sys":
            # The bindings and sdl2 share the Rust-SDL2 project's MIT notice; the
            # -sys crate also carries SDL's Zlib notice under SDL/LICENSE.txt.
            rust_sdl = next(c for c in crates if c["name"] == "sdl2" and c["version"] == version)
            found += [Path(rust_sdl["manifest_path"]).parent / "LICENSE", folder / "SDL/LICENSE.txt"]
        if not found:
            raise ValueError(f"Missing Rust license text for {name} {version}")
        for path in found:
            add(f"{name} {version}, {path.name}", path)

    rust = subprocess.check_output(["rustc", "--version"], text=True).split()[1]
    rust_doc = Path("/usr/share/doc") / f"libstd-rust-{'.'.join(rust.split('.')[:2])}-dev/copyright"
    if not rust_doc.is_file():
        raise ValueError(f"Missing Ubuntu Rust standard library copyright at {rust_doc}")
    add(f"Ubuntu Rust standard library ({rust})", rust_doc)

    sections = [(ROOT / "packaging/THIRD-PARTY.md").read_text(encoding="utf-8"), "Source inventory\n" + "\n".join(inventory)]
    sections += ["\n".join(names) + "\n\n" + body for body, names in texts.items()]
    return ("\n\n" + "=" * 72 + "\n\n").join(sections) + "\n"


if __name__ == "__main__":
    out = Path(sys.argv[1])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(collect(), encoding="utf-8")
