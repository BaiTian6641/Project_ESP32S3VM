#!/usr/bin/env python3
"""Copy owned firmware and existing pinned Arduino into native source snapshots.

Consumes an already prepared canonical SDK manifest. No SDK activation,
package install, network download, compilation or runtime check is performed.
Original sources and historical build directories remain untouched.
"""
import argparse
import hashlib
import json
import os
import pathlib
import shutil
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[4]
MODES = ("connected", "absent", "disconnected", "wrong", "uhci", "arduino")
ARDUINO_COMMIT = "94afccf35fb1e401facddbcf9e13bcf7c76a31d8"
IDF_COMMITS = {"idf-6.1": "fff9895c82d744c7237be8847347bdd1b07c6643",
               "idf-5.5.5": "b774170ff46c393eeb5e495ea37936038d3f4f4f"}


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1048576), b""):
            h.update(block)
    return h.hexdigest()


def component_files(root):
    # Hash the exact materialized compiler input, including directory links.
    # Cyclic links cannot be copied into a finite native source snapshot.
    for directory, dirs, names in os.walk(root, followlinks=True):
        dirs[:] = sorted(name for name in dirs if name != ".git")
        current = pathlib.Path(directory)
        real = current.resolve()
        parent = current.parent
        while current != root and parent != root.parent:
            require(real != parent.resolve(), f"Cyclic component directory link: {current}")
            parent = parent.parent
        for name in sorted(names):
            path = current / name
            if name != ".git" and path.is_file():
                yield path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=MODES, required=True)
    parser.add_argument("--sdk-manifest", type=pathlib.Path, required=True)
    parser.add_argument("--idf-source", type=pathlib.Path, required=True)
    parser.add_argument("--build-root", type=pathlib.Path, required=True)
    parser.add_argument("--arduino-source", type=pathlib.Path)
    args = parser.parse_args()
    sdk = json.loads(args.sdk_manifest.read_text())
    profile = "idf-5.5.5" if args.mode == "arduino" else "idf-6.1"
    require(sdk["profile"] == profile and sdk["commit"] == IDF_COMMITS[profile], "Canonical SDK manifest profile/commit mismatch")
    require(pathlib.Path(sdk["build_root"]).resolve() == args.build_root.resolve(), "Build root differs from canonical SDK contract")
    require(pathlib.Path(sdk["source"]).resolve() == args.idf_source.resolve(),
            "Activated SDK source differs from canonical ready manifest")
    original = ROOT / "tests/firmware/uart_native"
    if args.mode == "arduino":
        original /= "arduino"
    fixture_files = [original / "CMakeLists.txt"]
    fixture_files.extend(sorted(original.glob("sdkconfig*.defaults")))
    fixture_files.extend(sorted(path for path in (original / "main").rglob("*") if path.is_file()))
    fixture_hashes = {str(path.relative_to(original)): digest(path) for path in fixture_files}
    arduino_hashes = {}
    arduino = None
    if args.mode == "arduino":
        require(args.arduino_source is not None, "Arduino profile needs the existing component source")
        arduino = args.arduino_source.resolve()
        revision = subprocess.check_output(["git", "-C", str(arduino), "rev-parse", "HEAD"], text=True).strip()
        require(revision == ARDUINO_COMMIT, "Original Arduino component differs from locked3.3.12 commit")
        arduino_hashes = {str(path.relative_to(arduino)): digest(path) for path in component_files(arduino)}
    identity = dict(mode=args.mode, sdk_profile=profile, sdk_commit=sdk["commit"],
                    fixture_files=fixture_hashes, arduino_commit=ARDUINO_COMMIT if arduino else None,
                    arduino_files=arduino_hashes)
    identity["sdk_manifest_sha256"] = digest(args.sdk_manifest)
    source_hash = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    snapshot = args.build_root.resolve() / "uart_native" / f"{args.mode}-{source_hash}"
    source_dir = snapshot / "source"
    arduino_dir = snapshot / "arduino" if arduino else None
    manifest_path = snapshot / "source-manifest.json"
    if snapshot.exists():
        require(manifest_path.is_file(), "Existing source snapshot is incomplete; preserve it and choose a fresh native build root")
        manifest = json.loads(manifest_path.read_text())
        require(manifest["source_sha256"] == source_hash and manifest["identity"] == identity,
                "Existing source snapshot identity differs; refusing to rewrite it")
        for relative, sha in manifest["files"].items():
            require(digest(snapshot / relative) == sha, f"Existing native source snapshot changed: {relative}")
    else:
        snapshot.mkdir(parents=True)
        for path in fixture_files:
            target = source_dir / path.relative_to(original)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
        if arduino is not None:
            # Preserve the complete existing component, including its original
            # Git metadata. No source edits/submodule update/download occurs.
            # Dereference links so compiler includes never return to DrvFS.
            shutil.copytree(arduino, arduino_dir, symlinks=False)
        files = {f"source/{relative}": sha for relative, sha in fixture_hashes.items()}
        files.update({f"arduino/{relative}": sha for relative, sha in arduino_hashes.items()})
        # Detect a source change during copying before any delegated compile.
        for relative, sha in files.items():
            require(digest(snapshot / relative) == sha, f"Source changed while snapshotting: {relative}")
        manifest = dict(schema_version=1, source_sha256=source_hash, identity=identity,
                        sdk_manifest_sha256=digest(args.sdk_manifest), sdk_manifest=str(args.sdk_manifest.resolve()),
                        fixture_original=str(original), fixture_snapshot=str(source_dir),
                        arduino_original=str(arduino) if arduino else None,
                        arduino_snapshot=str(arduino_dir) if arduino_dir else None,
                        files=files, qualification="Source identity only; no compile/runtime success implied")
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    # Four stable lines are consumed by the owned Bash builder with mapfile.
    print(source_dir)
    print(snapshot / "build")
    print(arduino_dir or "")
    print(manifest_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
