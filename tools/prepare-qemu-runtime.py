#!/usr/bin/env python3
"""Prepare one content-addressed native checkout from reviewed runtime inputs.

The official source and existing prototype lanes are never modified. Reuse is
allowed only when every applied source hash and checkout edit matches the record.
New inputs get a new checkout, rather than reconciling or overwriting old edits.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
EXTENSIONS = ROOT / "qemu-extensions"


def sha(data):
    return hashlib.sha256(data).hexdigest()


def relative_path(value):
    path = PurePosixPath(value)
    if not value or path.is_absolute() or ".." in path.parts or "\\" in value or ":" in value:
        raise ValueError(f"Expected a relative source path: {value}")
    return path.as_posix()


def git(source, *args, timeout=120, data=None):
    result = subprocess.run(["git", "-C", str(source), *args], input=data,
                            capture_output=True, timeout=timeout)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace").strip())
    return result.stdout


def changes(source):
    return git(source, "status", "--porcelain=v1", "--untracked-files=all", "-z",
               "--", ".", ":(exclude)build-runtime").split(b"\0")


def check_owned_changes(source, allowed):
    for entry in changes(source):
        if not entry:
            continue
        status, path = entry[:2].decode("ascii"), entry[3:].decode("utf-8")
        if status not in {" M", "??"} or path not in allowed:
            raise ValueError(f"Unrecorded, staged or renamed edit; preserving checkout: {path}")


def prepare(cache_root, profile_path):
    profile = json.loads(profile_path.read_text(encoding="utf-8"))
    lock = json.loads((ROOT / "runtime-lock.json").read_text(encoding="utf-8"))
    base_commit = lock["qemu"]["commit"]
    if profile.get("schema_version") != 1 or profile.get("base_commit") != base_commit:
        raise ValueError("Runtime profile does not match the locked official base")
    patch_context = profile.get("patch_context", "strict")
    if patch_context not in {"strict", "ignore-space-change"}:
        raise ValueError(f"Unsupported patch context policy: {patch_context}")
    apply_options = ["--ignore-space-change"] if patch_context == "ignore-space-change" else []
    baseline = (ROOT / "build-qemu-official-base").resolve(strict=True)
    if git(baseline, "rev-parse", "HEAD").decode().strip() != base_commit:
        raise ValueError("Official source differs from the lock; leaving it untouched")

    inputs, destinations, patch_blobs = {}, {}, []
    for item in profile["copies"]:
        name, destination = relative_path(item["source"]), relative_path(item["destination"])
        if name in inputs or destination in destinations:
            raise ValueError("Duplicate copied source or destination")
        path = (EXTENSIONS / name).resolve(strict=True)
        if not path.is_relative_to(EXTENSIONS.resolve()):
            raise ValueError(f"Source escapes tracked extensions: {name}")
        inputs[name] = path.read_bytes().replace(b"\r\n", b"\n")
        destinations[destination] = inputs[name]
    allowed = set(destinations)
    for item in profile["patches"]:
        name = relative_path(item["source"])
        if name in inputs:
            raise ValueError(f"Duplicate runtime input: {name}")
        path = (EXTENSIONS / name).resolve(strict=True)
        if not path.is_relative_to(EXTENSIONS.resolve()):
            raise ValueError(f"Patch escapes tracked extensions: {name}")
        # Patch bytes are applied exactly as generated: some committed QEMU
        # sources are CRLF, and their lane patches carry CRLF context that a
        # normalized copy could never match (git am needs --keep-cr too).
        inputs[name] = path.read_bytes()
        targets = {relative_path(target) for target in item["targets"]}
        actual = {line.split("\t", 2)[2] for line in
                  git(baseline, "apply", "--numstat", "-", data=inputs[name]).decode().splitlines()}
        if not targets or actual != targets:
            raise ValueError(f"Patch scope differs from reviewed runtime profile: {name}")
        allowed.update(targets)
        patch_blobs.append(inputs[name])

    input_hashes = {name: sha(data) for name, data in sorted(inputs.items())}
    identity = {"profile": profile, "inputs": input_hashes}
    fingerprint = sha(json.dumps(identity, sort_keys=True, separators=(",", ":")).encode())
    source = (cache_root / f"qemu-runtime-{base_commit[:12]}-{fingerprint[:16]}").resolve()
    if source == baseline or source.is_relative_to(baseline):
        raise ValueError("Runtime destination must be separate from the official source")
    record_path = ROOT / "build-runtime-state" / f"runtime-{fingerprint[:16]}-source.json"

    if source.exists():
        if not record_path.is_file():
            raise ValueError(f"Unrecorded existing checkout; preserve it: {source}")
        record = json.loads(record_path.read_text(encoding="utf-8"))
        if record["source"] != str(source) or record["fingerprint"] != fingerprint:
            raise ValueError("Runtime provenance does not match this checkout")
        if record["inputs"] != input_hashes or set(record["applied_files"]) != allowed:
            raise ValueError("Runtime inputs or owned file set differ from recorded provenance")
        if git(source, "rev-parse", "HEAD").decode().strip() != base_commit:
            raise ValueError("Runtime checkout HEAD changed; preserving it")
        for name, expected in record["applied_files"].items():
            if sha((source / name).read_bytes()) != expected:
                raise ValueError(f"Runtime source changed; refusing overwrite: {name}")
        check_owned_changes(source, allowed)
        return source

    source.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "clone", "--no-hardlinks", "--no-checkout",
         str(baseline), str(source)], capture_output=True, timeout=600)
    if result.returncode:
        raise ValueError(result.stderr.decode(errors="replace"))
    git(source, "-c", "core.autocrlf=false", "checkout", "--detach", base_commit)
    if any(changes(source)):
        raise ValueError("New runtime source is not pristine; preserving it")
    # Scope-check and apply the same frozen bytes, not reopened patch paths.
    # A worker editing an input during cloning cannot change the applied series
    # without changing its recorded fingerprint.
    patch_data = b"\n".join(patch_blobs)
    git(source, "apply", "--check", *apply_options, "-", data=patch_data)
    git(source, "apply", *apply_options, "-", data=patch_data)
    for name, data in destinations.items():
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    check_owned_changes(source, allowed)
    record = {"schema_version": 1, "created_at": datetime.now(timezone.utc).isoformat(),
              "source": str(source), "base_commit": base_commit, "fingerprint": fingerprint,
              "profile": profile, "inputs": input_hashes,
              "applied_files": {name: sha((source / name).read_bytes()) for name in sorted(allowed)},
              "qualification": "Source prepared; combined build and behavior verification required"}
    record_path.parent.mkdir(parents=True, exist_ok=True)
    temporary = record_path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    temporary.replace(record_path)
    return source


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-root", type=Path,
                        default=Path.home() / ".cache/esp32s3vm")
    parser.add_argument("--profile", type=Path,
                        default=EXTENSIONS / "runtime-profile.json")
    args = parser.parse_args()
    print(prepare(args.cache_root, args.profile))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(f"Runtime preparation refused: {error}", file=sys.stderr)
        sys.exit(1)
