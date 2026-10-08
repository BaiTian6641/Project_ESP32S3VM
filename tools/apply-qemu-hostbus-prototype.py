#!/usr/bin/env python3
"""Apply tracked prototype inputs to a separate pinned checkout, preserving edits."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
PROTOTYPE = ROOT / "qemu-extensions/prototypes/hostbus"
COPIES = {"hostbus-probe.c": "backends/hostbus-probe.c",
          "hostbus-probe.h": "include/sysemu/hostbus-probe.h",
          "hostbus-json.h": "backends/hostbus-json.h"}
PATCH_SCOPE = {"integration.patch": {"backends/meson.build", "system/cpus.c"},
               "gdb-execution-preflight.patch": {"gdbstub/gdbstub.c"},
               "state-restore-guard.patch": {"migration/migration.c", "migration/savevm.c",
                                              "migration/migration-hmp-cmds.c"},
               "qom-options.patch": {"qapi/qom.json"},
               "creation-order.patch": {"system/vl.c"}}


def git(source, *args):
    result = subprocess.run(["git", "-C", str(source), *args], capture_output=True)
    if result.returncode:
        raise ValueError("Git source guard failed: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    pristine = (ROOT / "build-qemu-official-base").resolve()
    if source == pristine or source.is_relative_to(pristine):
        raise ValueError("Prototype must never modify the official baseline checkout")
    expected = json.loads((ROOT / "runtime-lock.json").read_text())["qemu"]["commit"]
    mapping = json.loads((PROTOTYPE / "source-map.json").read_text())
    if mapping["base_commit"] != expected or {item["source"]: item["destination"] for item in mapping["copies"]} != COPIES:
        raise ValueError("Prototype source map differs from reviewed integration scope")
    if git(source, "rev-parse", "HEAD").decode().strip() != expected:
        raise ValueError("Extension checkout differs from pinned base")
    if Path(git(source, "rev-parse", "--show-toplevel").decode().strip()).resolve() != source:
        raise ValueError("Expected an independent extension checkout root")
    patch_names = [mapping["integration_patch"], *mapping.get("additional_patches", [])]
    if patch_names != list(PATCH_SCOPE):
        raise ValueError("Expected the complete reviewed integration/debugger/restore patch sequence")
    inputs = {name: (PROTOTYPE / name).read_bytes().replace(b"\r\n", b"\n")
              for name in [*COPIES, *patch_names]}
    common = set().union(*(PATCH_SCOPE[name] for name in patch_names))
    for name in patch_names:
        paths = {line.split("\t", 2)[2] for line in
                 git(source, "apply", "--numstat", str(PROTOTYPE / name)).decode().splitlines()}
        if paths != PATCH_SCOPE[name]:
            raise ValueError(f"Patch changes files outside the reviewed scope: {name}")
    record_path = ROOT / "build-runtime-state/hostbus-extension-source.json"
    # This script creates only this generated build directory. Exclude its
    # objects/logs from the source-edit guard; every other path stays checked.
    status = git(source, "status", "--porcelain=v1", "--untracked-files=all", "-z",
                 "--", ".", ":(exclude)build-hostbus")
    changes = [entry.decode() for entry in status.split(b"\0") if entry]
    previous = {"inputs": {}, "applied_files": {}}
    if changes:
        if not record_path.exists():
            raise ValueError("Existing unrecorded edits: preserve them and prepare another checkout")
        previous = json.loads(record_path.read_text())
        if previous["source"] != str(source) or previous["base_commit"] != expected:
            raise ValueError("Recorded checkout identity differs; preserve existing edits")
        allowed = common | set(COPIES.values())
        if any(entry[3:] not in allowed or entry[:2] not in {" M", "??"} for entry in changes):
            raise ValueError("Unrelated/staged/renamed edits: refusing prototype overwrite")
        for name, sha in previous["applied_files"].items():
            if digest((source / name).read_bytes()) != sha:
                raise ValueError(f"Existing extension edit differs from recorded source: {name}")
    pending = []
    for name in patch_names:
        if name in previous["inputs"]:
            if previous["inputs"][name] != digest(inputs[name]):
                raise ValueError(f"Applied common patch changed; use a fresh checkout: {name}")
            git(source, "apply", "--reverse", "--check", str(PROTOTYPE / name))
        else:
            for path in PATCH_SCOPE[name]:
                if (source / path).read_bytes() != git(source, "show", f"HEAD:{path}"):
                    raise ValueError(f"New guard patch requires pristine source: {path}")
            git(source, "apply", "--check", str(PROTOTYPE / name))
            pending.append(name)
    # All scopes and existing hashes have been checked before the first mutation.
    for name in pending:
        git(source, "apply", str(PROTOTYPE / name))
    for name, destination in COPIES.items():
        target = source / destination
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(inputs[name])
    record = {"schema_version": 1, "created_at": datetime.now(timezone.utc).isoformat(),
              "source": str(source), "base_commit": expected, "license": "GPL-2.0-or-later",
              "inputs": {name: digest(data) for name, data in inputs.items()},
              "applied_files": {name: digest((source / name).read_bytes())
                                for name in sorted(common | set(COPIES.values()))},
              "qualification": "source applied; build and scheduler qualification pending"}
    record_path.parent.mkdir(parents=True, exist_ok=True)
    record_path.write_text(json.dumps(record, indent=2) + "\n")
    (record_path.parent / "extension-checkout.json").write_text(json.dumps({
        "schema_version": 1, "source": str(source), "base_commit": expected,
        "patches": [{"name": name, "sha256": digest(inputs[name])} for name in patch_names],
        "qualification": record["qualification"]}, indent=2) + "\n")
    print(f"Recorded separate extension inputs: {record_path}")


if __name__ == "__main__":
    main()
