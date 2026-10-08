#!/usr/bin/env python3
"""Complete an aborted full native suite without repeating observed results.

Usage: native_qtest_collect.py --source FINAL_IMMUTABLE_PACK --prior-result
  FAILED_NATIVE_RESULT --evidence NEW_DIRECTORY
The existing full-suite failure remains a failure. Every registered case not
already attempted is run once in an isolated process; the union is the entire
frozen suite, never a selected subset. No builds or source writes occur here.
"""
import argparse
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

from native_build_verify import require, sha256, source_identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("source", "prior-result", "evidence"):
        parser.add_argument(f"--{option}", type=Path, required=True)
    parser.add_argument("--case-timeout", type=float, default=180)
    parser.add_argument("--registered-cases-log", type=Path,
                        help="reuse actual discovery output without repeating the listing command")
    parser.add_argument("--binary-receipt", type=Path,
                        help="new compiled QTest-only repair receipt for explicitly changed failed cases")
    parser.add_argument("--case", action="append", default=[],
                        help="changed failed case to recheck; requires --binary-receipt")
    parser.add_argument("--runtime-delta", action="store_true",
                        help="explicitly approved own I2S C/header repair with a freshly rebuilt runtime")
    args = parser.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=False)
    result = dict(status="FAIL", cases=[], hardware_used=False, commands=[])
    try:
        prior = json.loads(args.prior_result.read_text())
        current = source_identity(args.source.resolve())
        compiled = json.loads(args.binary_receipt.read_text()) if args.binary_receipt else prior
        require(current == compiled["source_before"], "authoritative full source differs from the compiled binary receipt")
        result["verification_scope"] = compiled.get("verification_scope", "i2s-native-prototype")
        result["current_parent_foundation_qualified"] = False
        if args.binary_receipt:
            previous = prior["source"]
            require(previous["base_commit"] == current["base_commit"] and
                    previous["prefix_fingerprint"] == current["prefix_fingerprint"],
                    "failed-check repair changed the runtime base")
            changed = {name for name in set(previous["targets"]) | set(current["targets"])
                       if previous["targets"].get(name) != current["targets"].get(name)}
            if args.runtime_delta:
                allowed = {"hw/misc/esp32s3_i2s.c", "include/hw/misc/esp32s3_i2s.h",
                           "tests/qtest/esp32s3-i2s-test.c"}
                require("hw/misc/esp32s3_i2s.c" in changed and changed <= allowed,
                        "runtime failed-check repair exceeds the approved own I2S source scope")
                require(compiled["verification_scope"] == "i2s-specific-historical-runtime-delta",
                        "runtime delta lacks its explicit whole-pack source/compile scope")
            else:
                require(changed == {"tests/qtest/esp32s3-i2s-test.c"},
                        "selected host repair requires unchanged runtime and QTest-only source delta")
            result["repair_binary_receipt"] = dict(path=str(args.binary_receipt.resolve()),
                                                   sha256=sha256(args.binary_receipt))
        else:
            require(not args.case and not args.runtime_delta, "repair checks require an actual rebuilt binary receipt")
        result["source"] = current
        result["prior_result"] = dict(path=str(args.prior_result.resolve()), sha256=sha256(args.prior_result))
        binaries = compiled["binaries"]
        qemu = next(Path(path) for path in binaries if Path(path).name == "qemu-system-xtensa")
        qtest = next(Path(path) for path in binaries if Path(path).name == "esp32s3-i2s-test")
        for path in (qemu, qtest):
            require(sha256(path) == binaries[str(path)]["sha256"], f"compiled binary changed: {path}")
        result["binaries"] = binaries
        execution_directory = Path(compiled["commands"][0]["cwd"])
        result["execution_directory"] = str(execution_directory)
        if args.binary_receipt:
            previous_core = next(entry for path, entry in prior["binaries"].items()
                                 if Path(path).name == "qemu-system-xtensa")
            if args.runtime_delta:
                require(previous_core["sha256"] != binaries[str(qemu)]["sha256"],
                        "runtime source repair requires a genuinely relinked new core")
            else:
                require(previous_core["sha256"] == binaries[str(qemu)]["sha256"],
                        "selected host repair cannot inherit results from a different runtime binary")
            registered = prior["registered_cases"]
            failures = set(prior.get("remaining_prior_failed_cases", prior.get("prior_failed_cases", []))) | set(prior["new_failed_cases"])
            selected = set(args.case)
            require(selected and len(selected) == len(args.case) and selected <= failures,
                    "repair checks must be distinct, explicitly changed previously failed cases")
            observed = set(registered) - selected
            result["selected_changed_failed_cases"] = sorted(selected)
        else:
            original_log = args.prior_result.parent / "qtest-all.log"
            text = original_log.read_text(errors="replace")
            observed = set(re.findall(r"^(?:ok|not ok)(?:\s+\d+)?\s+(/[^\s]+)", text, re.MULTILINE))
            failures = set(re.findall(r"^not ok(?:\s+\d+)?\s+(/[^\s]+)", text, re.MULTILINE))
            require(observed and failures, "prior log must retain an actual attempted suite failure")
        environment = dict(os.environ, QTEST_QEMU_BINARY=str(qemu))
        (evidence / "environment.json").write_text(json.dumps(environment, indent=2) + "\n")
        listing_command = [str(qtest), "-l"]
        if args.binary_receipt:
            listing_bytes = (args.prior_result.parent / "registered-cases.log").read_bytes()
            result["case_discovery_reused_from_prior_suite"] = True
        elif args.registered_cases_log:
            listing_bytes = args.registered_cases_log.read_bytes()
            result["reused_discovery"] = dict(path=str(args.registered_cases_log.resolve()),
                                             sha256=sha256(args.registered_cases_log))
        else:
            listing = subprocess.run(listing_command, cwd=execution_directory, env=environment,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            require(listing.returncode == 0, "registered-case discovery failed")
            listing_bytes = listing.stdout
        (evidence / "registered-cases.log").write_bytes(listing_bytes)
        registered = re.findall(r"^(?:# )?(/[^\s]+)$", listing_bytes.decode(errors="replace"), re.MULTILINE)
        require(len(registered) == current["qtest_cases"] and len(set(registered)) == len(registered),
                "registered cases differ from the entire frozen native suite")
        require(observed <= set(registered), "prior attempted case is not in the frozen suite")
        result.update(registered_cases=registered, prior_attempted_cases=sorted(observed),
                      prior_failed_cases=sorted(failures), case_discovery_command=listing_command)
        for index, case in enumerate(registered):
            if case in observed:
                continue
            command = [str(qtest), "--verbose", "-p", case]
            entry = dict(case=case, command=command, started_ns=time.time_ns(),
                         log=f"case-{index:03d}.log")
            result["cases"].append(entry)
            with (evidence / entry["log"]).open("wb") as log:
                process = subprocess.Popen(command, cwd=execution_directory, env=environment,
                                           stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
                try:
                    process.wait(args.case_timeout)
                    entry["returncode"] = process.returncode
                except subprocess.TimeoutExpired:
                    entry["timeout"] = True
                finally:
                    try:
                        os.killpg(process.pid, signal.SIGTERM)
                    except ProcessLookupError:
                        pass
                    if process.poll() is None:
                        try:
                            process.wait(10)
                        except subprocess.TimeoutExpired:
                            os.killpg(process.pid, signal.SIGKILL)
                            process.wait()
            output = (evidence / entry["log"]).read_text(errors="replace")
            passed = re.findall(r"^ok(?:\s+\d+)?\s+(/[^\s]+)", output, re.MULTILINE)
            entry.update(ended_ns=time.time_ns(), log_sha256=sha256(evidence / entry["log"]),
                         passed=entry.get("returncode") == 0 and passed == [case] and
                                not re.search(r"#\s*SKIP\b", output, re.IGNORECASE))
        result["attempted_union_count"] = len(observed) + len(result["cases"])
        require(result["attempted_union_count"] == current["qtest_cases"], "complete native attempt coverage missing")
        result["new_failed_cases"] = [entry["case"] for entry in result["cases"] if not entry["passed"]]
        result["new_passed_cases"] = [entry["case"] for entry in result["cases"] if entry["passed"]]
        if args.binary_receipt:
            result["remaining_prior_failed_cases"] = sorted(failures - set(args.case))
            result["status"] = "CHANGED_FAILED_CHECKS_PASS_PRIOR_FAILURES_REMAIN" if not result["new_failed_cases"] else "CHANGED_FAILED_CHECKS_FAIL"
        else:
            result["status"] = "COMPLETE_NATIVE_SUITE_ATTEMPT_WITH_FAILURES"
    except Exception as exc:
        result["error"] = repr(exc)
    finally:
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence))))
    return 0 if result["status"] == "CHANGED_FAILED_CHECKS_PASS_PRIOR_FAILURES_REMAIN" else 1


if __name__ == "__main__":
    raise SystemExit(main())
