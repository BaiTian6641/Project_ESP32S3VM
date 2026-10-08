#!/usr/bin/env python3
"""Run only after native/dependency READY. Never rebuild another worker lane."""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import time
import xml.etree.ElementTree as ET


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--checkout", required=True)
    ap.add_argument("--repo", required=True)
    ap.add_argument("--evidence", required=True)
    ap.add_argument("--adc-build", required=True, help="Read-only ordinary ADC owner's IDF build directory")
    ap.add_argument("--sdk-activation", required=True, help="Prepared native locked IDF6.1 activate.sh")
    ap.add_argument("--source-record", required=True, help="Combined's immutable canonical source receipt")
    args = ap.parse_args()
    repo = pathlib.Path(args.repo).resolve()
    checkout = pathlib.Path(args.checkout).resolve()
    evidence = pathlib.Path(args.evidence).resolve()
    source_record = pathlib.Path(args.source_record).resolve()
    provenance = json.loads(source_record.read_text())
    assert pathlib.Path(provenance["source"]).resolve() == checkout, provenance["source"]
    evidence.mkdir(parents=True, exist_ok=True)
    build = checkout / "build-runtime"
    build.mkdir(exist_ok=True)
    qemu = build / "qemu-system-xtensa"
    runs = []
    report = dict(checkout=str(checkout), runs=runs, limits=[
        "No physical board, GUI relinking, Wi-Fi/radio electrical qualification or nonlinear device support.",
        "Finite GPIO profile:40ohm push-pull/low open-drain; enabled firmware pulls45kohm; VIL0.25/VHI0.75.",
        "Source-only factor reuse means unchanged MNA matrix; GPIO polarity changes physical rail endpoints and may refactor.",
        "RC suite is permitted only against the parent READY corrected native and vendored kernel."])
    report["source_fingerprint"] = provenance["fingerprint"]
    report["source_record"] = str(source_record)

    def run(label, command, cwd=repo, env=None, timeout=600, required=True):
        merged = os.environ.copy()
        if env:
            merged.update(env)
        start = time.monotonic()
        with (evidence / f"{label}.log").open("w") as log:
            log.write("COMMAND " + json.dumps(command) + "\nCWD " + str(cwd) + "\n")
            log.flush()
            try:
                result = subprocess.run(command, cwd=cwd, env=merged, stdout=log, stderr=subprocess.STDOUT, timeout=timeout)
                code = result.returncode
            except subprocess.TimeoutExpired:
                code = "timeout"
        runs.append(dict(label=label, command=command, cwd=str(cwd), returncode=code,
                         duration_seconds=round(time.monotonic() - start, 3), pass_=code == 0))
        (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n")
        if required and code != 0:
            raise RuntimeError(f"{label}: {code}; see evidence")
        return code

    try:
        if not (build / "build.ninja").exists():
            run("configure", ["bash", "../configure", "--target-list=xtensa-softmmu", "--enable-gcrypt", "--enable-slirp",
                "--disable-docs", "--disable-werror", "--disable-user", "--disable-tools", "--disable-guest-agent",
                "--disable-gtk", "--disable-sdl", "--disable-vnc", "--disable-opengl", "--disable-capstone"], cwd=build, timeout=600)
        suites = ["electrical", "gpio", "adc", "coreclk", "gdma", "intmatrix"]
        targets = [f"tests/qtest/esp32s3-{suite}-test" for suite in suites]
        run("build-native", ["ninja", "-j", "4", "qemu-system-xtensa", *targets], cwd=build, timeout=1800)
        for suite, target in zip(suites, targets):
            run(f"qtest-{suite}", [str(build / target), "--verbose"], cwd=build,
                env={"QTEST_QEMU_BINARY": str(qemu)}, required=False)
        # Numerical targets compile the exact vendored bytes in this binary's
        # immutable source, not whatever a mutable sibling kernel later becomes.
        numerical_source = evidence / "numerical-source"
        (numerical_source / "tests").mkdir(parents=True, exist_ok=True)
        for stem in ("net-dc", "net-adapter", "net-rc"):
            for extension in (".c", ".h"):
                name = stem + extension
                (numerical_source / name).write_bytes((checkout / "hw/adc" / name).read_bytes())
        electrical = repo / "qemu-extensions/electrical"
        (numerical_source / "CMakeLists.txt").write_bytes((electrical / "CMakeLists.txt").read_bytes())
        for test in ("net-dc-test.c", "net-adapter-test.c", "net-rc-test.c"):
            (numerical_source / "tests" / test).write_bytes((electrical / "tests" / test).read_bytes())
        cbuild = evidence / "electrical-cmake"
        run("configure-kernels", ["cmake", "-S", str(numerical_source), "-B", str(cbuild), "-G", "Ninja"], timeout=180)
        run("build-kernels", ["cmake", "--build", str(cbuild), "--parallel", "4"], timeout=300)
        junit = evidence / "numerical-junit.xml"
        run("ctest-six", ["ctest", "--test-dir", str(cbuild), "--output-on-failure", "--output-junit", str(junit)], timeout=300, required=False)
        cases = list(ET.parse(junit).getroot().iter("testcase")) if junit.exists() else []
        report["numerical"] = dict(tests=len(cases),
                                   failures=sum(case.find("failure") is not None for case in cases),
                                   skipped=sum(case.find("skipped") is not None for case in cases))
        fixture = repo / "qemu-extensions/prototypes/electrical-runtime/firmware"
        export = pathlib.Path(args.sdk_activation).resolve()
        sdk_manifest = export.parent / "sdk-manifest.json"
        sdk = json.loads(sdk_manifest.read_text())
        assert sdk["profile"] == "idf-6.1" and sdk["target"] == "esp32s3", sdk
        idf_build = pathlib.Path(sdk["build_root"]) / "electrical-runtime" / evidence.name
        idf_build.mkdir(parents=True, exist_ok=True)
        report["sdk"] = sdk
        report["idf_build"] = str(idf_build)
        run("build-idf61", ["bash", "-lc", f'source "{export}" && idf.py -B "{idf_build}" -D "SDKCONFIG={idf_build / "sdkconfig"}" -D IDF_TARGET=esp32s3 build'], cwd=fixture, timeout=1800)
        flash = evidence / "native-electrical.merged.bin"
        run("merge-idf61", ["bash", "-lc", f'source "{export}" && python -m esptool --chip esp32s3 merge_bin --fill-flash-size 4MB -o "{flash}" --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 esp32s3_native_electrical.bin'], cwd=idf_build, timeout=180)
        run("ordinary-idf61", ["python3", str(fixture / "run-native-fixture.py"), "--qemu", str(qemu), "--flash", str(flash), "--evidence", str(evidence / "fixture")], timeout=180, required=False)
        adc_build = pathlib.Path(args.adc_build).resolve()
        adc_flash = evidence / "adc-oneshot-native.merged.bin"
        run("merge-adc-idf61", ["bash", "-lc", f'source "{export}" && python -m esptool --chip esp32s3 merge_bin --fill-flash-size 4MB -o "{adc_flash}" --flash-mode dio --flash-freq 80m --flash-size 4MB 0x0 bootloader/bootloader.bin 0x8000 partition_table/partition-table.bin 0x10000 esp32s3_adc_oneshot.bin'], cwd=adc_build, timeout=180)
        run("ordinary-adc-idf61", ["python3", str(fixture / "run-native-adc.py"), "--qemu", str(qemu), "--flash", str(adc_flash), "--evidence", str(evidence / "adc-fixture")], timeout=180, required=False)
        boot = repo / "gui-esp32s3-simulator/build-wsl/gui_esp32s3_boot_smoke_test"
        boot_images = {
            "idf-6.1": repo / "tests/firmware/boot_smoke/build-idf-6.1/boot-smoke.merged.bin",
            "idf-5.5.5": repo / "tests/firmware/boot_smoke/build-idf-5.5.5/boot-smoke.merged.bin",
            "arduino-3.3.12": repo / "tests/firmware/arduino_boot/build-idf-5.5.5/arduino-boot.merged.bin",
        }
        for profile, image in boot_images.items():
            run(f"boot-control-{profile}", [str(boot)], env={"ESP32S3_QEMU_BIN": str(qemu), "ESP32S3_BOOT_FIRMWARE": str(image), "QT_QPA_PLATFORM": "offscreen"}, timeout=120, required=False)
        artifacts = [qemu, flash, adc_flash, adc_build / "esp32s3_adc_oneshot.elf", idf_build / "esp32s3_native_electrical.elf", export, sdk_manifest, source_record, boot, *boot_images.values()]
        artifacts += [build / target for target in targets]
        artifacts += list(fixture.rglob("*.py")) + list((fixture / "main").glob("*"))
        artifacts += [fixture / "sdkconfig.defaults", fixture / "CMakeLists.txt"]
        artifacts += list((repo / "qemu-extensions/prototypes/electrical-runtime/copies").glob("*"))
        artifacts += list((checkout / "hw/adc").glob("*electrical*"))
        artifacts += list((checkout / "hw/adc").glob("*project*"))
        artifacts += list((checkout / "hw/adc").glob("net-*"))
        artifacts += list(numerical_source.glob("*")) + list((numerical_source / "tests").glob("*"))
        report["sha256"] = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in artifacts if path.is_file()}
        numerical_complete = report["numerical"] == dict(tests=6, failures=0, skipped=0)
        report["status"] = "PASS" if all(r["pass_"] for r in runs) and numerical_complete else "FAIL"
    except Exception as exc:
        report["status"] = "FAIL"
        report["error"] = repr(exc)
        raise
    finally:
        (evidence / "result.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
