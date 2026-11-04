#!/usr/bin/env python3
"""Assemble and verify a versioned SDK containing only Direct Rime inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

from build import LIBRARIES, LOCK, sha256


def archive_objects(path):
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"Invalid archive: {path}")
    offset = 8
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[58:60] != b"`\n":
            raise ValueError(f"Invalid ar member: {path}")
        size = int(header[48:58])
        member = data[offset + 60:offset + 60 + size]
        if len(member) != size:
            raise ValueError(f"Truncated ar member: {path}")
        if member.startswith(b"\x7fELF"):
            yield member
        offset += 60 + size + size % 2


def verify(root, commit):
    lock = json.loads(LOCK.read_text())
    architectures = {"armeabi-v7a": (1, 40), "arm64-v8a": (2, 183), "x86": (1, 3), "x86_64": (2, 62)}
    manifests = []
    for abi in lock["abis"]:
        manifest = json.loads((root / "librime" / abi / "share/rime/source-manifest.json").read_text())
        if manifest["librime_commit"] != commit or manifest["abi"] != abi:
            raise ValueError(f"Wrong SDK source/ABI: {abi}")
        if manifest["sources_lock_sha256"] != sha256(LOCK) or manifest["sources"] != lock["sources"]:
            raise ValueError(f"Wrong dependency sources: {abi}")
        for key in ("ndk", "cmake", "platform"):
            if manifest[key] != lock[key]:
                raise ValueError(f"Wrong toolchain: {abi}/{key}")
        if manifest["link_smoke_test"] != "passed":
            raise ValueError(f"Missing final link verification: {abi}")
        for name, library in LIBRARIES.items():
            path = root / name / abi / "lib" / library
            digest = manifest["archive_sha256"] if name == "librime" else manifest["dependency_archives_sha256"][name]
            if sha256(path) != digest:
                raise ValueError(f"Archive checksum mismatch: {path}")
            objects = list(archive_objects(path))
            if not objects or any((obj[4], struct.unpack_from("<H", obj, 18)[0]) != architectures[abi] for obj in objects):
                raise ValueError(f"Wrong object architecture: {path}")
        header = root / "librime" / abi / "include/rime_api.h"
        if sha256(header) != manifest["rime_api_sha256"]:
            raise ValueError(f"API checksum mismatch: {abi}")
        if any(api not in header.read_text() for api in ("get_input_tabs", "free_input_tabs", "select_tab", "clear_tabs", "delete_candidate")):
            raise ValueError(f"Missing custom public API: {abi}")
        manifests.append(manifest)
    if len({m["rime_api_sha256"] for m in manifests}) != 1:
        raise ValueError("Inconsistent public API across ABIs")
    for path in (root / "opencc/data").glob("*.json"):
        config = json.loads(path.read_text())
        def dictionaries(value):
            if isinstance(value, dict):
                if "file" in value:
                    yield value["file"]
                for child in value.values():
                    yield from dictionaries(child)
            elif isinstance(value, list):
                for child in value:
                    yield from dictionaries(child)
        for name in dictionaries(config):
            if not (path.parent / name).is_file():
                raise ValueError(f"Missing OpenCC dictionary: {name}")
    if not (root / "opencc/data/s2t.json").is_file() or not (root / "licenses/librime-LICENSE").is_file():
        raise ValueError("Missing shared data/licenses")
    return manifests


def package(root, output, commit):
    manifests = verify(root, commit)
    # CMake package files and host tools contain build-machine paths and are
    # unnecessary for the consumer. Ship explicit libraries, public headers,
    # generated dictionaries, licenses and provenance only.
    paths = set()
    for abi in json.loads(LOCK.read_text())["abis"]:
        for name, library in LIBRARIES.items():
            paths.add(root / name / abi / "lib" / library)
            paths.update(p for p in (root / name / abi / "include").rglob("*") if p.is_file())
        paths.add(root / "librime" / abi / "share/rime/source-manifest.json")
    paths.update(p for folder in ("opencc/data", "licenses") for p in (root / folder).rglob("*") if p.is_file())
    names = {p.relative_to(root).as_posix(): p for p in paths}
    record = {
        "schema_version": 1, "librime_commit": commit,
        "upstream_commit": manifests[0]["upstream_commit"],
        "abis": json.loads(LOCK.read_text())["abis"],
        "sources_lock_sha256": sha256(LOCK),
        "files": {name: sha256(path) for name, path in sorted(names.items())},
    }
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name, path in sorted(names.items()):
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o100644 << 16
            archive.writestr(info, path.read_bytes())
        archive.writestr("sdk-manifest.json", json.dumps(record, indent=2) + "\n")
    output.with_name("SHA256SUMS").write_text(f"{sha256(output)}  {output.name}\n")
    output.with_name("sdk-manifest.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"asset": output.name, "sha256": sha256(output), "files": len(names), "bytes": output.stat().st_size}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("product", type=Path)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.output:
        package(args.product, args.output, args.commit)
    else:
        verify(args.product, args.commit)
        print("Four ABI archives, API, toolchain, dependencies and shared data verified")
