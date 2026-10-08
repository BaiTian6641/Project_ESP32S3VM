#!/usr/bin/env python3
"""Run the unmodified ADC owner's ordinary IDF image against native v3 nets."""
import argparse
import hashlib
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import time

spec = importlib.util.spec_from_file_location("native_fixture", pathlib.Path(__file__).with_name("run-native-fixture.py"))
shared = importlib.util.module_from_spec(spec)
spec.loader.exec_module(shared)


def project():
    def comp(cid, kind, terminals, parameters=None):
        return dict(id=cid, name=cid, type=kind, kind=kind, terminals=terminals, parameters=parameters or {})
    def q(value, unit):
        return dict(value=value, unit=unit)
    t = shared.terminal
    components = [comp("U1", "mcu", [t("U1", "vdd", "power"), t("U1", "gnd", "ground"),
                  t("U1", "io3", "analog", 3), t("U1", "io4", "analog", 4), t("U1", "io11", "analog", 11)]),
                  comp("G", "ground", [t("G", "ref", "ground")]),
                  comp("V", "voltage-source", [t("V", "p", "power"), t("V", "n", "ground")], {"voltage": q(3.3, "V")}),
                  comp("Vlow", "voltage-source", [t("Vlow", "p", "power"), t("Vlow", "n", "ground")], {"voltage": q(1.8, "V")})]
    endpoints = {"gnd": ["G.ref", "V.n", "Vlow.n", "U1.gnd"], "vdd": ["V.p", "U1.vdd"], "lowrail": ["Vlow.p"]}
    for gpio in (3, 4, 11):
        for half in ("top", "bottom"):
            cid = f"R{gpio}{half}"
            components.append(comp(cid, "resistor", [t(cid, "a", "passive"), t(cid, "b", "passive")], {"resistance": q(10000, "ohm")}))
        endpoints["vdd" if gpio == 3 else "lowrail"].append(f"R{gpio}top.a")
        endpoints["gnd"].append(f"R{gpio}bottom.b")
        endpoints[f"tap{gpio}"] = [f"R{gpio}top.b", f"R{gpio}bottom.a", f"U1.io{gpio}"]
    return dict(version=3, id="native-adc-idf61", name="Native ADC ordinary IDF6.1",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"), firmware={},
                runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")), components=components,
                nets=[dict(id=cid, name=cid, endpoints=ends) for cid, ends in endpoints.items()], geometry=dict(components={}, nets={}))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--flash", required=True)
    ap.add_argument("--evidence", required=True)
    args = ap.parse_args()
    evidence = pathlib.Path(args.evidence).resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    socket_dir = pathlib.Path(tempfile.mkdtemp(dir="/tmp", prefix="s3-adc-"))
    sock = socket_dir / "qmp.sock"
    uart = socket_dir / "uart.log"
    archived_uart = evidence / "adc-uart.log"
    command = [args.qemu, "-machine", "esp32s3", "-nographic", "-S", "-serial", f"file:{uart}",
               "-drive", f"file={args.flash},if=mtd,format=raw", "-qmp", f"unix:{sock},server=on,wait=off"]
    document = project()
    (evidence / "project.json").write_text(json.dumps(document, indent=2) + "\n")
    report = dict(command=command, expected_codes={"adc1_ch2": 3079, "adc1_ch3": 3350, "adc2_ch0": 3350})
    qmp = None
    with (evidence / "adc-stderr.log").open("w") as log:
        proc = subprocess.Popen(command, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 20
            while not sock.exists() and time.monotonic() < deadline:
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited {proc.returncode}")
                time.sleep(.05)
            qmp = shared.Qmp(sock)
            qmp.apply(document)
            report["initial_snapshot"] = qmp.snapshot()
            for pad in report["initial_snapshot"]["pads"]:
                expected = 1.65 if pad["gpio"] == 3 else .9
                assert pad["valid"] and abs(pad["voltage_v"] - expected) < 1e-9, pad
            qmp.call("cont")
            text = shared.wait_for(uart, lambda value: "ADC_FX done" in value, seconds=60)
            codes = re.findall(r"ADC_FX (adc\d_ch\d+) raw=(\d+) valid=1", text)
            assert len(codes) == 9, text
            for channel, expected in report["expected_codes"].items():
                observed = [int(raw) for name, raw in codes if name == channel]
                assert observed == [expected] * 3, (channel, observed, expected)
            assert not re.search(r"ADC_FX.*err=0x[1-9a-f]", text), text
            qmp.call("stop")
            report["final_snapshot"] = qmp.snapshot()
            report["native_sample_provider"] = qmp.call("qom-get", dict(path="/machine/soc/sens", property="sample-provider"))
            assert report["native_sample_provider"] == "/machine/soc/electrical", report["native_sample_provider"]
            report["observed_codes"] = codes
            report["status"] = "PASS"
        except Exception as exc:
            report["status"] = "FAIL"
            report["error"] = repr(exc)
            if qmp is not None:
                for key, capture in (("failure_runstate", lambda: qmp.call("query-status")),
                                     ("failure_snapshot", qmp.snapshot)):
                    try:
                        report[key] = capture()
                    except Exception as diagnostic:
                        report[key + "_capture_error"] = repr(diagnostic)
            raise
        finally:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            if qmp is not None:
                qmp.close()
            # Retain this native directory if durable archiving fails.
            if uart.exists():
                archived_uart.write_bytes(uart.read_bytes())
            report["sha256"] = {str(p): hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()
                                for p in (args.qemu, args.flash, archived_uart, evidence / "project.json") if pathlib.Path(p).exists()}
            (evidence / "adc-result.json").write_text(json.dumps(report, indent=2) + "\n")
            shutil.rmtree(socket_dir)


if __name__ == "__main__":
    main()
