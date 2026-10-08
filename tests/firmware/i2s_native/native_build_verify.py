#!/usr/bin/env python3
"""Build a published immutable full I2S checkout and run its entire native suite.

Usage (Linux/WSL): python3 native_build_verify.py --source OWNER_CHECKOUT
  --evidence NEW_DIRECTORY [--jobs 4]
The checkout must already have been prepared by the source owner. This driver
never applies patches, repairs source, touches a corpus, or builds a prefix.
Every command/environment/output is retained, including failed attempts.
After an owner-published QTest-only source repair, --reuse-build OLD_BUILD
--prior-result FAILED_RESULT reuses identical core objects, binds the repaired
test compilation to the new authoritative pack, and preserves failed receipts.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

CONFIGURE = ["--target-list=xtensa-softmmu", "--enable-gcrypt", "--enable-slirp",
             "--disable-docs", "--disable-werror", "--disable-user", "--disable-tools",
             "--disable-guest-agent", "--disable-gtk", "--disable-sdl", "--disable-vnc",
             "--disable-opengl", "--disable-capstone", "--extra-cflags=-fno-pie",
             "--extra-ldflags=-no-pie"]


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def source_identity(source):
    pack = source / ".esp32s3vm-i2s-inputs"
    ledger = json.loads((pack / "prepared-source.json").read_text())
    mapping = json.loads((pack / "source-map.json").read_text())
    require(mapping["status"] == "source-ready", "owner SOURCE_READY unavailable")
    require(not ledger["prefix_only"] and not ledger["clock_prefix_only"] and not ledger["foundations_only"],
            "prefix/foundations are not full I2S source")
    require(ledger["source_map_sha256"] == sha256(pack / "source-map.json"), "archived source map changed")
    for operation in ledger["operations"]:
        require(sha256(source / operation["snapshot"]) == operation["sha256"],
                f"immutable input changed: {operation['snapshot']}")
    for name, digest in ledger["targets"].items():
        require(sha256(source / name) == digest, f"published source changed: {name}")
    cases = mapping["i2s_source_freeze"]["qtest_cases"]
    require(type(cases) is int and cases >= 57, "complete original and metadata suite freeze missing")
    qtest = source / "tests/qtest/esp32s3-i2s-test.c"
    require(sha256(qtest) == mapping["i2s_source_freeze"]["qtest_sha256"], "QTest source freeze mismatch")
    return dict(prepared_source_sha256=sha256(pack / "prepared-source.json"),
                source_map_sha256=sha256(pack / "source-map.json"), targets=ledger["targets"],
                base_commit=ledger["base_commit"], prefix_fingerprint=ledger["prefix_fingerprint"],
                qtest_cases=cases)


def rebind_compile(command, source, cache_build):
    rebound = []
    for argument in command:
        if argument.startswith("-I../"):
            argument = "-I" + str(source / argument[5:])
        elif argument == "-I..":
            argument = "-I" + str(source)
        elif argument.startswith("../"):
            argument = str(source / argument[3:])
        else:
            argument = argument.replace(str(cache_build.parent) + "/", str(source) + "/")
        rebound.append(argument)
    return rebound


def rebuild_runtime_delta(source, cache_build, evidence, run, changed):
    """Rebuild the compiler-recorded header ABI closure, never only the leaf C."""
    run("generated-runtime-commands", ["ninja", "-t", "commands", "qemu-system-xtensa",
                                        "tests/qtest/esp32s3-i2s-test"])
    raw_commands = (evidence / "generated-runtime-commands.log").read_text().splitlines()
    commands = [shlex.split(line) for line in raw_commands]
    run("compiler-dependency-graph", ["ninja", "-t", "deps"])
    dependencies = {}
    output = None
    root = str(cache_build.parent) + "/"

    def relative(path):
        if path.startswith(root):
            return path[len(root):]
        if path.startswith("../"):
            return os.path.normpath(path[3:])
        return None

    for line in (evidence / "compiler-dependency-graph.log").read_text().splitlines():
        match = re.match(r"^(.+): #deps ", line)
        if match:
            output = match[1]
            dependencies[output] = set()
        elif output and line.startswith("    "):
            path = relative(line.strip())
            if path is not None:
                dependencies[output].add(path)
    affected = []
    for command in commands:
        if "-c" not in command or "-o" not in command:
            continue
        output = command[command.index("-o") + 1]
        input_path = relative(command[command.index("-c") + 1])
        closure = dependencies.get(output, set())
        if input_path in changed or closure & changed:
            affected.append(dict(output=output, source=input_path, command=command,
                                 changed_dependencies=sorted(closure & changed)))
    sources = {entry["source"] for entry in affected}
    require({"hw/misc/esp32s3_i2s.c", "hw/xtensa/esp32s3.c"} <= sources,
            "header ABI closure must rebuild both I2S and its embedding SoC")
    require("tests/qtest/esp32s3-i2s-test.c" in sources, "repaired QTest compile missing")
    (evidence / "affected-compile-units.json").write_text(json.dumps(affected, indent=2) + "\n")
    for index, entry in enumerate(affected):
        run(f"compile-affected-{index:02d}", rebind_compile(entry["command"], source, cache_build))
    outputs = {entry["output"] for entry in affected}
    for index, (raw, command) in enumerate(zip(raw_commands, commands)):
        if "ar" in command and any(output in command for output in outputs):
            run(f"reindex-affected-archive-{index}", ["/bin/sh", "-c", raw])
    destination = source / "build-i2s-native"
    (destination / "tests/qtest").mkdir(parents=True, exist_ok=False)
    (destination / "pc-bios").symlink_to("../pc-bios", target_is_directory=True)
    binaries = []
    for name in ("qemu-system-xtensa", "tests/qtest/esp32s3-i2s-test"):
        matching = [command for command in commands
                    if "-o" in command and command[command.index("-o") + 1] == name and "-c" not in command]
        require(len(matching) == 1, f"actual generated link command is not unique: {name}")
        command = list(matching[0])
        binary = destination / name
        command[command.index("-o") + 1] = str(binary)
        run("link-runtime-core" if name == "qemu-system-xtensa" else "link-runtime-qtest", command)
        binaries.append(binary)
    return binaries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--reuse-build", type=Path)
    parser.add_argument("--prior-result", type=Path)
    parser.add_argument("--historical-pack", action="store_true",
                        help="Main-approved whole historical dependency pack; never current foundation qualification")
    parser.add_argument("--build-only", action="store_true",
                        help="compile/link a source repair; run only its affected failed checks separately")
    parser.add_argument("--runtime-delta", action="store_true",
                        help="authorized own I2S C/header delta: rebuild recorded dependency closure and emit new binaries")
    args = parser.parse_args()
    source, evidence = args.source.resolve(), args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    result = dict(status="FAIL", commands=[], hardware_used=False,
                  verification_scope="i2s-specific-historical-runtime-delta" if args.runtime_delta else
                                     "i2s-specific-historical-pack" if args.historical_pack else "i2s-native-prototype",
                  current_parent_foundation_qualified=False)
    build = args.reuse_build.resolve() if args.reuse_build else source / "build-i2s-native"
    try:
        result["source_before"] = source_identity(source)
        cases = result["source_before"]["qtest_cases"]
        result["qtest_expected_cases"] = cases
        require(args.jobs > 0, "jobs must be positive")
        if args.reuse_build:
            require(args.prior_result is not None, "cache reuse requires the original failed result")
            prior = json.loads(args.prior_result.read_text())
            configure_receipt = prior
            while "compiled_cache_cutover" in configure_receipt:
                link = configure_receipt["compiled_cache_cutover"]
                path = Path(link["prior_result"])
                require(sha256(path) == link["prior_result_sha256"], "preserved prior receipt changed")
                configure_receipt = json.loads(path.read_text())
            original = configure_receipt["source_before"]
            current = result["source_before"]
            require(original["base_commit"] == current["base_commit"] and
                    original["prefix_fingerprint"] == current["prefix_fingerprint"],
                    "compiled cache base differs from authoritative full source")
            changed = {name for name in set(original["targets"]) | set(current["targets"])
                       if original["targets"].get(name) != current["targets"].get(name)}
            allowed = {"tests/qtest/esp32s3-i2s-test.c"}
            if args.runtime_delta:
                require(args.historical_pack, "runtime delta requires the authorized entire historical dependency pack")
                allowed |= {"hw/misc/esp32s3_i2s.c", "include/hw/misc/esp32s3_i2s.h"}
            require(changed == allowed, f"source cutover differs from its explicit authorized scope: {sorted(changed)}")
            require(any(command["name"] == "configure" and command["returncode"] == 0 and
                        Path(command["cwd"]).resolve() == build for command in configure_receipt["commands"]),
                    "original configure was not successful in the reused build")
            cache_source = source_identity(build.parent)
            require(cache_source == original, "cache source no longer matches the preserved failed receipt")
            result["compiled_cache_cutover"] = dict(
                prior_result=str(args.prior_result.resolve()), prior_result_sha256=sha256(args.prior_result),
                cache_build=str(build), cache_source=cache_source, changed_targets=sorted(changed),
                source_authority="source_before is the new immutable full pack, never the cache")
        else:
            require(args.prior_result is None and not args.runtime_delta,
                    "incremental receipt/delta requires --reuse-build")
            build.mkdir(exist_ok=False)
        environment = dict(os.environ)
        (evidence / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")

        def run(name, command, env=environment):
            entry = dict(name=name, command=command, cwd=str(build), started_ns=time.time_ns(),
                         environment_file="environment.json", environment_overrides={
                             key: value for key, value in env.items() if environment.get(key) != value})
            result["commands"].append(entry)
            with (evidence / f"{name}.log").open("wb") as log:
                completed = subprocess.run(command, cwd=build, env=env, stdout=log, stderr=subprocess.STDOUT)
            entry.update(returncode=completed.returncode, ended_ns=time.time_ns(),
                         log_sha256=sha256(evidence / f"{name}.log"))
            require(completed.returncode == 0, f"{name} failed: see {name}.log")

        runtime_binaries = None
        if args.runtime_delta:
            old_core = build / "qemu-system-xtensa"
            require(sha256(old_core) == prior["binaries"][str(old_core)]["sha256"],
                    "runtime delta preimage no longer matches the preserved binary receipt")
            result["compiled_cache_cutover"]["preimage_core"] = prior["binaries"][str(old_core)]
            runtime_binaries = rebuild_runtime_delta(source, build, evidence, run, changed)
        elif args.reuse_build:
            run("generated-qtest-commands", ["ninja", "-t", "commands", "tests/qtest/esp32s3-i2s-test"])
            commands = [shlex.split(line) for line in
                        (evidence / "generated-qtest-commands.log").read_text().splitlines()]

            def output_command(output):
                matching = [command for command in commands
                            if "-o" in command and command[command.index("-o") + 1] == output]
                require(len(matching) == 1, f"generated command for {output} is not unique")
                return matching[0]

            compile_command = output_command("tests/qtest/esp32s3-i2s-test.p/esp32s3-i2s-test.c.o")
            require("../tests/qtest/esp32s3-i2s-test.c" in compile_command,
                    "generated command does not compile the expected QTest source")
            run("compile-repaired-qtest", rebind_compile(compile_command, source, build))
            core = build / "qemu-system-xtensa"
            core_receipt = prior.get("binaries", {}).get(str(core))
            if core_receipt:
                require(sha256(core) == core_receipt["sha256"], "unchanged compiled core binary changed")
                result["compiled_cache_cutover"]["reused_core_binary"] = core_receipt
            else:
                run("finish-core-cache", ["ninja", "-j", str(args.jobs), "qemu-system-xtensa"])
            run("link-repaired-qtest", output_command("tests/qtest/esp32s3-i2s-test"))
        else:
            run("configure", [str(source / "configure"), *CONFIGURE])
            run("build", ["ninja", "-j", str(args.jobs), "qemu-system-xtensa", "tests/qtest/esp32s3-i2s-test"])
        binary, qtest = runtime_binaries if runtime_binaries else (build / "qemu-system-xtensa", build / "tests/qtest/esp32s3-i2s-test")
        result["binaries"] = {str(path): dict(sha256=sha256(path), bytes=path.stat().st_size)
                              for path in (binary, qtest)}
        if args.build_only:
            result["qtest_executed"] = False
        else:
            result["qtest_executed"] = True
            testenv = dict(environment, QTEST_QEMU_BINARY=str(binary))
            run("qtest-all", [str(qtest), "--verbose"], testenv)
            text = (evidence / "qtest-all.log").read_text(errors="replace")
            passed = re.findall(r"^ok\s+\d+\s+(.+)$", text, re.MULTILINE)
            require(len(passed) == cases, f"full QTest count is {len(passed)}, expected {cases}")
            require(not re.search(r"^not ok\b", text, re.MULTILINE), "QTest reports a failing case")
            require(not re.search(r"^ok\b.*#\s*SKIP\b", text, re.MULTILINE | re.IGNORECASE),
                    "skipped cases are not full native qualification")
            result["qtest_passed"] = passed
        result["source_after"] = source_identity(source)
        require(result["source_after"] == result["source_before"], "source changed during build/test")
        result["status"] = "PASS_NATIVE_BUILD_ONLY_NO_TEST_QUALIFICATION" if args.build_only else "PASS_NATIVE_BUILD_QTESTS"
    except Exception as exc:
        result["error"] = repr(exc)
    finally:
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence))))
    return 0 if result["status"] in ("PASS_NATIVE_BUILD_QTESTS", "PASS_NATIVE_BUILD_ONLY_NO_TEST_QUALIFICATION") else 1


if __name__ == "__main__":
    raise SystemExit(main())
