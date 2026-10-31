"""Write the license notices for exactly the dependency revisions used by this build."""
import argparse
import json
from pathlib import Path
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
TARGET = "aarch64-unknown-linux-gnu"


def collect(upstream, root=ROOT):
    # Keep shared texts (in particular Apache-2.0) once, with attribution for every package.
    texts = {}
    inventory = []

    def add(label, path):
        body = path.read_text(encoding="utf-8-sig").strip()
        if not body:
            raise ValueError(f"Empty notice for {label}")
        texts.setdefault(body, []).append(label)

    for path in ("LICENSE", "NOTICE.md"):
        add(f"YOUYOU / UURemote-ns, {path}", root / path)
    add("Noto Sans SC (Copyright 2014-2021 Adobe; Noto is a trademark of Google Inc.)",
        upstream / "assets/fonts/OFL.txt")

    upstream_ref = (upstream / ".youyou-linux-upstream").read_text().strip()
    inventory.append(f"Native client: https://github.com/doke69/YOUYOU-Remote-ns/tree/{upstream_ref}")
    native = json.loads((upstream / "scripts/sources.lock.json").read_text())
    linked = {"libdatachannel", "libjuice", "libsrtp", "mbedtls", "json", "usrsctp", "plog"}
    for dep in native:
        if dep["name"] not in linked:
            continue
        folder = upstream / "external" / dep["path"]
        inventory.append(f'{dep["name"]}  https://github.com/{dep["repo"]}/tree/{dep["ref"]}')
        notices = [p for p in sorted(folder.iterdir()) if p.is_file() and p.name.upper().startswith(("LICENSE", "COPYING", "NOTICE"))]
        if not notices:
            raise ValueError(f'Missing native notices for {dep["name"]}')
        for path in notices:
            add(f'{dep["name"]}, {path.name}', path)
    # This declaration is in HACL*'s source header, the Apache terms appear in Mbed TLS' LICENSE.
    inventory.append("Mbed TLS Everest/HACL*: Copyright (c) INRIA and Microsoft Corporation. All rights reserved. Apache-2.0.")

    metadata = json.loads(subprocess.check_output(
        ["cargo", "metadata", "--offline", "--locked", "--filter-platform", TARGET, "--format-version", "1"],
        cwd=root,
    ))
    locked = tomllib.loads((root / "Cargo.lock").read_text())
    sums = {(p["name"], p["version"]): p["checksum"] for p in locked["package"] if "checksum" in p}
    crates = [p for p in metadata["packages"] if p.get("source")]
    for p in sorted(crates, key=lambda p: (p["name"], p["version"])):
        name, version = p["name"], p["version"]
        folder = Path(p["manifest_path"]).parent
        inventory.append(f'{name} {version}  {p["license"]}  https://crates.io/crates/{name}/{version}  SHA256 {sums[(name, version)]}')
        notices = [p for p in sorted(folder.iterdir()) if p.is_file() and p.name.upper().startswith(("LICENSE", "COPYING", "NOTICE", "COPYRIGHT"))]
        if name == "sdl2-sys":
            # The bindings and sdl2 share the Rust-SDL2 project's MIT notice; the
            # -sys crate also carries SDL's Zlib notice under SDL/LICENSE.txt.
            rust_sdl = next(c for c in crates if c["name"] == "sdl2" and c["version"] == version)
            notices.extend([Path(rust_sdl["manifest_path"]).parent / "LICENSE", folder / "SDL/LICENSE.txt"])
        if not notices:
            raise ValueError(f"Missing Rust license text for {name} {version}")
        for path in notices:
            add(f"{name} {version}, {path.name}", path)

    rust_ver = subprocess.check_output(["rustc", "--version"], text=True).split()[1]
    rust_doc = Path("/usr/share/doc") / f"libstd-rust-{'.'.join(rust_ver.split('.')[:2])}-dev/copyright"
    if rust_doc.is_file():
        add(f"Ubuntu Rust standard library ({rust_ver})", rust_doc)
    else:
        raise ValueError(f"Missing Ubuntu Rust standard library copyright at {rust_doc}")

    preamble = (root / "packaging/THIRD-PARTY.md").read_text()
    sections = [preamble, "Source inventory\n" + "\n".join(inventory)]
    sections.extend("\n".join(names) + "\n\n" + body for body, names in texts.items())
    return ("\n\n" + "=" * 72 + "\n\n").join(sections) + "\n"


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    result = collect(args.upstream)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding="utf-8")
