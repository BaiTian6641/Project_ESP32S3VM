#!/usr/bin/env python3
"""Record executable, source and firmware identity without claiming qualification."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import platform
import re
import subprocess


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def command(args, cwd=None, timeout=20):
    result = subprocess.run(args, cwd=cwd, capture_output=True, text=True, timeout=timeout, check=True)
    return result.stdout.strip()


def git_head(source):
    """Read a checkout's HEAD without scanning its working tree or starting Git."""
    marker = source / ".git"
    if marker.is_file():
        declaration = marker.read_text(encoding="utf-8").strip()
        if not declaration.startswith("gitdir: "):
            raise ValueError(f"Invalid Git metadata marker: {marker}")
        git_dir = (source / declaration[8:]).resolve(strict=True)
    elif marker.is_dir():
        git_dir = marker
    else:
        raise ValueError(f"Submodule checkout is not initialized: {source}")
    value = (git_dir / "HEAD").read_text(encoding="utf-8").strip()
    if value.startswith("ref: "):
        reference = value[5:]
        if not reference.startswith("refs/") or ".." in Path(reference).parts:
            raise ValueError("Invalid symbolic Git HEAD")
        common = git_dir
        if (git_dir / "commondir").is_file():
            common = (git_dir / (git_dir / "commondir").read_text().strip()).resolve(strict=True)
        for directory in (git_dir, common):
            path = directory / reference
            if path.is_file():
                value = path.read_text(encoding="utf-8").strip()
                break
        else:
            packed = common / "packed-refs"
            if not packed.is_file():
                raise ValueError(f"Missing Git reference: {reference}")
            found = [line.split(" ", 1)[0] for line in packed.read_text().splitlines()
                     if line.endswith(" " + reference) and not line.startswith(("#", "^"))]
            if len(found) != 1:
                raise ValueError(f"Missing/ambiguous packed Git reference: {reference}")
            value = found[0]
    if not re.fullmatch(r"[0-9a-f]{40}", value):
        raise ValueError(f"Invalid Git commit identity: {source}")
    return value


def submodule_revisions(source):
    """Verify expected gitlinks against actual HEADs, including nested modules.

    `git submodule status --recursive` spawns many working-tree Git operations on
    /mnt/c. Query only each parent object's named gitlinks, then read child HEAD
    metadata directly. Missing/mismatched modules fail rather than losing evidence.
    """
    records = []

    def collect(parent):
        metadata = parent / ".gitmodules"
        if not metadata.is_file():
            return
        # Git permits repeated vendor metadata such as IDF 5.5.5's sbom-cpe.
        # Identity-bearing paths must remain unambiguous; never take the last
        # duplicate path and silently omit a different gitlink from evidence.
        config = subprocess.run(
            ["git", "config", "--file", str(metadata), "--null",
             "--get-regexp", r"^submodule\..*\.path$"],
            cwd=parent, capture_output=True, timeout=20)
        if config.returncode == 1:
            return  # No submodule path entries.
        config.check_returncode()
        paths, keys = [], set()
        for entry in config.stdout.split(b"\0"):
            if not entry:
                continue
            key, path = entry.decode("utf-8").split("\n", 1)
            if key in keys or path in paths:
                raise ValueError(f"Duplicate submodule path: {key}")
            keys.add(key)
            paths.append(path)
        result = subprocess.run(["git", "ls-tree", "-z", "HEAD", "--", *paths], cwd=parent,
                                capture_output=True, timeout=20, check=True)
        links = {}
        for entry in result.stdout.split(b"\0"):
            if not entry:
                continue
            header, path = entry.split(b"\t", 1)
            mode, kind, revision = header.decode("ascii").split()
            if mode == "160000" and kind == "commit":
                links[path.decode("utf-8")] = revision
        for name in paths:
            child = (parent / name).resolve(strict=True)
            if not child.is_relative_to(source) or name not in links:
                raise ValueError(f"Invalid submodule gitlink/path: {name}")
            actual = git_head(child)
            if actual != links[name]:
                raise ValueError(f"Submodule {child} differs from its pinned gitlink")
            records.append({"path": child.relative_to(source).as_posix(), "commit": actual,
                            "expected_commit": links[name]})
            collect(child)

    collect(source)
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--qemu", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--firmware", action="append", default=[], type=Path)
    parser.add_argument("--idf", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    binary = args.qemu.resolve(strict=True)
    manifest = {
        "schema_version": 1,
        "recorded_at": datetime.now(timezone.utc).isoformat(),
        "host": {"platform": platform.platform(), "architecture": platform.machine()},
        "qemu": {"path": str(binary), "sha256": digest(binary),
                 "version": command([str(binary), "--version"]).splitlines()[0],
                 "machine": "esp32s3"},
        "firmware": [],
        "qualification": "identity only; attach actual test results separately",
    }
    if args.source:
        source = args.source.resolve(strict=True)
        manifest["qemu"]["source"] = {
            "path": str(source),
            "commit": command(["git", "rev-parse", "HEAD"], source),
            "tracked_changes": command(["git", "status", "--porcelain", "--untracked-files=no"], source),
        }
    if args.idf:
        source = args.idf.resolve(strict=True)
        # Vendor library identity is tracked separately from the public framework.
        libraries = []
        for directory in (source / "components/esp_wifi", source / "components/esp_phy",
                          source / "components/esp_coex", source / "components/bt"):
            if directory.is_dir():
                for path in sorted(directory.rglob("*.a")):
                    if "esp32s3" in path.parts:
                        libraries.append({"path": str(path.relative_to(source)), "sha256": digest(path)})
        manifest["idf"] = {"path": str(source), "commit": command(["git", "rev-parse", "HEAD"], source),
                           "tools_manifest_sha256": digest(source / "tools/tools.json"),
                           "submodule_revisions": submodule_revisions(source),
                           "vendor_libraries": libraries}
    for path in args.firmware:
        path = path.resolve(strict=True)
        manifest["firmware"].append({"path": str(path), "sha256": digest(path), "bytes": path.stat().st_size})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Recorded runtime identity: {args.output}")


if __name__ == "__main__":
    main()
