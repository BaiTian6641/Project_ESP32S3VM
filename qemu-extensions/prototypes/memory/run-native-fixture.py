#!/usr/bin/env python3
"""Run ordinary flash/PSRAM firmware on a fresh retained writable flash copy.

Every leaf runs all CPU/GDMA/cache assertions on initial boot, actual QMP reset,
and process relaunch. Private raw-partition and NVS blobs must advance on disk.
PSRAM remap/capacity-end/backing-reset and CPU-unmapped exception remain unqualified.
"""
import argparse
import errno
import hashlib
import json
import pathlib
import re
import struct
import shutil
import socket
import subprocess
import tempfile
import time

LOCKS = {
    "idf-5.5.5": "b774170ff46c393eeb5e495ea37936038d3f4f4f",
    "idf-6.1": "fff9895c82d744c7237be8847347bdd1b07c6643",
}
FIXTURE = pathlib.Path(__file__).resolve().parents[3] / "tests/firmware/memory_native"
# Native filesystem for artifacts a running QEMU is still writing.
LIVE_ROOT = pathlib.Path.home() / ".cache/esp32s3vm"


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def digest(path):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            sha.update(block)
    return sha.hexdigest()


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args], timeout=60)


def source_identity(source):
    names = git(source, "ls-files", "-z", "--cached", "--others", "--exclude-standard",
                "--", ".", ":(exclude)build-runtime")
    hashes = {}
    for name in sorted(set(names.decode().split("\0")) - {""}):
        path = source / name
        if path.is_file():
            hashes[name] = digest(path)
    return dict(path=str(source), commit=git(source, "rev-parse", "HEAD").decode().strip(),
                tracked_diff_sha256=hashlib.sha256(git(source, "diff", "--binary", "HEAD")).hexdigest(),
                file_sha256=hashes)


def pattern(seed, length=384):
    return bytes((i * 37 + seed) & 255 for i in range(length))


def fnv(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f"{value:08x}"


def persistent_pattern(phase):
    return bytes(((i * 73 + (i >> 3) * 29 + phase * 53) ^ (0xa7 + i * 11)) & 255
                 for i in range(769))


def flash_partitions(image):
    parts = {}
    for offset in range(0x8000, 0x9000, 32):
        magic, kind, subtype, address, size, label, flags = struct.unpack_from("<HBBII16sI", image, offset)
        if magic != 0x50aa:
            break
        name = label.split(b"\0", 1)[0].decode("ascii")
        require(flags == 0 and address + size <= len(image), f"Unexpected partition flags/range: {name}")
        parts[name] = dict(type=kind, subtype=subtype, address=address, size=size)
    require({"factory", "nvs", "mem_raw"} <= parts.keys(), "Missing private fixture partition table")
    require(parts["factory"]["type"] == 0 and parts["nvs"]["type"] == 1 and
            parts["nvs"]["subtype"] == 2 and parts["mem_raw"]["type"] == 0x40 and
            parts["mem_raw"]["size"] == 4096, "Unexpected fixture partition types/sizes")
    ranges = sorted((p["address"], p["address"] + p["size"]) for p in parts.values())
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])), "Overlapping partitions")
    return parts


def validate_persistence(text, image, phase, parts):
    require(re.search(rf"^MEMORY_NATIVE_PERSIST previous={phase - 1} next={phase} length=769 raw_offset=37$", text, re.M),
            "Missing monotonic flash-derived phase transition")
    for name, label in (("app", "factory"), ("nvs", "nvs"), ("raw", "mem_raw")):
        part = parts[label]
        require(re.search(rf"^MEMORY_NATIVE_PARTITION name={name} label={label} type={part['type']} subtype={part['subtype']} "
                          rf"address={part['address']:08x} size={part['size']:08x} encrypted=0$", text, re.M),
                f"Actual discovery differs from built partition table: {name}")
    rows = {name: (int(length), value, prefix) for name, length, value, prefix in
            re.findall(r"^MEMORY_NATIVE_BYTES name=(\w+) length=(\d+) fnv32=([0-9a-f]{8}) prefix=([0-9a-f]+)$", text, re.M)}
    expected = {"flash_first_header": image[:32],
                "app_first_header": image[parts["factory"]["address"]:parts["factory"]["address"] + 32],
                "raw_committed": persistent_pattern(phase), "nvs_committed": persistent_pattern(phase)}
    checks = {"flash_partitions_found", "flash_partitions_safe", "flash_size_4m", "flash_headers",
              "raw_erase", "raw_write", "raw_committed", "nvs_init", "nvs_open", "nvs_set_blob",
              "nvs_set_phase", "nvs_commit", "nvs_reopen", "nvs_committed", "nvs_blob_length"}
    if phase == 1:
        expected["raw_fresh"] = bytes([255]) * 769
        checks |= {"nvs_fresh", "raw_fresh"}
    else:
        expected.update(raw_previous=persistent_pattern(phase - 1), nvs_previous=persistent_pattern(phase - 1))
        checks |= {"nvs_previous_phase", "nvs_phase_range", "raw_previous", "nvs_previous"}
    for name, data in expected.items():
        require(rows.get(name) == (len(data), fnv(data), data[:16].hex()), f"Flash/NVS bytes differ: {name}")
    passed = set(re.findall(r"^MEMORY_NATIVE_CHECK name=(\w+) result=PASS$", text, re.M))
    require(checks <= passed, f"Missing flash/NVS assertions: {sorted(checks - passed)}")
    return dict(previous=phase - 1, committed=phase, samples={name: rows[name] for name in expected})


class Qmp:
    def __init__(self, port, proc, transcript, deadline):
        self.transcript = transcript
        self.sequence = 0
        self.events = []
        while True:
            try:
                self.sock = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                require(proc.poll() is None, "QEMU exited before QMP connection")
                if time.monotonic() >= deadline:
                    raise TimeoutError("QMP connection watchdog")
                time.sleep(.05)
        self.sock.settimeout(max(.1, deadline - time.monotonic()))
        self.stream = self.sock.makefile("rb")
        require("QMP" in self.receive(), "Missing QMP greeting")
        self.command("qmp_capabilities")

    def record(self, direction, value):
        with self.transcript.open("a") as out:
            out.write(json.dumps(dict(host_monotonic=time.monotonic(), direction=direction, message=value)) + "\n")

    def receive(self):
        line = self.stream.readline()
        require(line, "QMP disconnected")
        value = json.loads(line)
        self.record("receive", value)
        if "event" in value:
            self.events.append(value)
        return value

    def command(self, name):
        self.sequence += 1
        request = dict(execute=name, id=self.sequence)
        self.record("send", request)
        self.sock.sendall(json.dumps(request).encode() + b"\r\n")
        while True:
            response = self.receive()
            if response.get("id") == self.sequence:
                require("return" in response, f"QMP command failed: {response}")
                return response

    def reset(self, watchdog):
        self.sock.settimeout(watchdog)
        start = len(self.events)
        self.command("system_reset")
        while not any(event.get("event") == "RESET" for event in self.events[start:]):
            self.receive()

    def close(self):
        self.stream.close()
        self.sock.close()


TRANSIENT_ERRNOS = frozenset((errno.ENODATA, errno.EAGAIN, errno.EBUSY, errno.ETXTBSY))


def read_guest_log(uart, offset=0, deadline=None):
    """Read the guest serial log while QEMU is still appending to it.

    A concurrently-appended file on a drvfs mount can fail with ENODATA instead
    of returning a partial file. Retry only that transient errno class (bounded,
    never past the caller's deadline); anything else is a real error.
    """
    for attempt in range(200):
        try:
            return uart.read_text(errors="replace")[offset:] if uart.exists() else ""
        except OSError as exc:
            if exc.errno not in TRANSIENT_ERRNOS or                     (deadline is not None and time.monotonic() >= deadline):
                raise
            time.sleep(.05)


def wait_boot(proc, uart, offset, watchdog):
    deadline = time.monotonic() + watchdog
    while True:
        text = read_guest_log(uart, offset, deadline)
        if re.search(r"^MEMORY_NATIVE_DONE [^\r\n]*\r?\n", text, re.M):
            require(len(re.findall(r"^MEMORY_NATIVE_BOOT ", text, re.M)) == 1, "Unexpected spontaneous reboot")
            return text
        require(proc.poll() is None, f"QEMU exited {proc.returncode} before complete consumer outcome")
        if time.monotonic() >= deadline:
            raise TimeoutError("Host diagnostic watchdog; no passing firmware outcome inferred")
        time.sleep(.05)


def disk_evidence(path, parts, phase=None):
    image = path.read_bytes()
    raw = parts["mem_raw"]
    if phase is not None:
        require(image[raw["address"] + 37:raw["address"] + 37 + 769] == persistent_pattern(phase),
                f"Raw phase {phase} not present in writable disk")
    return dict(sha256=digest(path), length=len(image),
                partition_sha256={name: hashlib.sha256(image[p["address"]:p["address"] + p["size"]]).hexdigest()
                                  for name, p in parts.items()})

def validate_uart(text, mode, idf_profile):
    # Shallow locked SDK checkouts without tags report the bare commit prefix
    # (e.g. idf=vfff9895c) instead of a tagged release; accept either the exact
    # release string or the exact locked commit prefix, never a wildcard.
    version = rf"(?:{re.escape(idf_profile[4:])}|{re.escape(LOCKS[idf_profile][:8])})"
    require(re.search(rf"^MEMORY_NATIVE_BOOT profile={mode} idf=v?{version}(?:[^\r\n]*)$", text, re.M),
            "UART profile or exact intended IDF release does not match")
    require(re.search(rf"^MEMORY_NATIVE_DONE profile={mode} failures=0 result=PASS$", text, re.M),
            "Firmware did not report a passing complete leaf")
    require(not re.search(r"^MEMORY_NATIVE_CHECK .*result=FAIL$", text, re.M), "Firmware failed a consumer assertion")
    checks = re.findall(r"^MEMORY_NATIVE_CHECK name=(\w+) result=(\w+)$", text, re.M)
    passed = {name for name, outcome in checks if outcome == "PASS"}
    expected = {"internal_descriptors", "internal_payloads", "sibling_pair", "internal_m2m", "internal_m2m_data",
                "disconnect_tx", "disconnect_rx", "delete_tx", "delete_rx", "supported_alignment"}
    samples = {"internal_m2m_data": 7}
    dma_names = {"internal_m2m": 0}
    if mode == "psram":
        require(re.search(r"^MEMORY_NATIVE_PSRAM initialized=1 capacity=8388608 id=425d0d mode=quad$", text, re.M),
                "PSRAM capacity or actual SSI RDID does not match genuine supported quad 8MiB part")
        alloc = re.search(r"^MEMORY_NATIVE_ALLOC src=(0x[0-9a-f]+) dst=(0x[0-9a-f]+) out_desc=(0x[0-9a-f]+) in_desc=(0x[0-9a-f]+)$", text, re.M)
        require(alloc, "Missing external payload/internal descriptor address evidence")
        src, dst, out_desc, in_desc = (int(x, 16) for x in alloc.groups())
        require(all(0x3c000000 <= x < 0x3e000000 for x in (src, dst)), "Payloads are not CPU PSRAM aliases")
        require(all(0x3fc88000 <= x < 0x3fd00000 for x in (out_desc, in_desc)), "Descriptors are not internal DRAM")
        expected |= {"psram_initialized", "psram_capacity_8m", "psram_real_id", "external_payloads", "dirty_cpu_new",
                     "dirty_dma_old", "writeback_dma_new", "prime_rx_cache", "stale_cpu_old", "physical_dma_new",
                     "invalidate_cpu_new", "boundary_allocation", "mmu_page_crossing_data", "unmapped_confirmed"}
        samples.update(dirty_dma_old=11, writeback_dma_new=71, physical_dma_new=97,
                       invalidate_cpu_new=97, mmu_page_crossing_data=153)
        dma_names.update(dirty_c2m_before=0, dirty_c2m_after=0, stale_m2c_before=0,
                         physical_rx_new=0, mmu_page_crossing=0, unbacked_tx_dscr_err=1, unbacked_rx_dscr_err=2)
        probe = re.search(r"^MEMORY_NATIVE_UNMAPPED address=(0x[0-9a-f]+) entry_invalid=1 translate_code=261$",
                          text, re.M)
        # The firmware proves the page unmapped from the MMU table itself; the
        # last DROM entry is excluded because IDF image_process owns it as a
        # transient boot-partition map page.
        require(probe and 0x3c000000 <= int(probe.group(1), 16) <= 0x3dff0000 and
                (int(probe.group(1), 16) - 0x3c000000) % 0x10000 == 0 and
                int(probe.group(1), 16) != 0x3dff0000,
                "Unbacked DMA target was not proved unmapped by esp_mmu_vaddr_to_paddr")
        require("MEMORY_NATIVE_SCOPE mmu_remap=qtest capacity_end=qtest psram_reset_persistence=qtest cpu_unmapped=not_qualified gdma_unmapped=qualified_if_pass octal=unsupported" in text,
                "Missing explicit unqualified scope")
        for burst in (16, 32, 64):
            require(re.search(rf"^MEMORY_NATIVE_ALIGNMENT burst={burst} external=1 internal_alignment=4 external_alignment={burst}$", text, re.M),
                    f"SDK did not report supported external alignment {burst}")
            for direction, seed in (("ext_to_ext", burst + 3), ("int_to_ext", burst + 121), ("ext_to_int", burst + 121)):
                name = f"{direction}_burst{burst}"
                dma_names[name] = 0
                samples[name] = seed
            dma_names[f"misaligned_rx_burst{burst}"] = 2
    else:
        require("MEMORY_NATIVE_SCOPE psram=not_qualified internal_leaf=only" in text,
                "Internal leaf incorrectly omitted PSRAM qualification exclusion")
        require("MEMORY_NATIVE_PSRAM" not in text, "Internal leaf made an external-memory claim")
    expected.update(dma_names)
    require(expected <= passed, f"Missing acceptance assertions: {sorted(expected - passed)}")
    rows = {name: (int(length), hash_value, prefix) for name, length, hash_value, prefix in
            re.findall(r"^MEMORY_NATIVE_BYTES name=(\w+) length=(\d+) fnv32=([0-9a-f]{8}) prefix=([0-9a-f]+)$", text, re.M)}
    for name, seed in samples.items():
        data = pattern(seed)
        require(rows.get(name) == (len(data), fnv(data), data[:16].hex()), f"Consumer bytes differ: {name}")
    states = {name: (int(tx, 16), int(rx, 16), int(nodes)) for name, tx, rx, nodes in
              re.findall(r"^MEMORY_NATIVE_DMA name=(\w+) tx_raw=([0-9a-f]{8}) rx_raw=([0-9a-f]{8}) nodes=(\d+)$", text, re.M)}
    for name, error in dma_names.items():
        require(name in states, f"Missing real GDMA status: {name}")
        tx, rx, nodes = states[name]
        require(nodes == 3, f"Missing multi-descriptor transaction: {name}")
        if error:
            require((tx & 4 if error == 1 else rx & 8) and not rx & 2, f"Expected DSCR_ERR absent or successful EOF present: {name}")
        else:
            require(rx & 2 and tx & 8 and not tx & 4 and not rx & 8, f"Real GDMA completion absent/error: {name}")
    return dict(checks=checks, samples=rows, gdma=states)


def provenance(args, evidence):
    metadata = json.loads(args.sdk_metadata.read_text())
    require(metadata["profile"] == args.idf_profile and metadata["commit"] == LOCKS[args.idf_profile], "SDK manifest is not the intended locked release")
    sdk = pathlib.Path(metadata["source"]).resolve(strict=True)
    require(git(sdk, "rev-parse", "HEAD").decode().strip() == LOCKS[args.idf_profile], "SDK HEAD differs from lock")
    require(not git(sdk, "diff", "HEAD", "--ignore-submodules=all"), "SDK source has tracked changes")
    build = args.build_dir
    description_path = build / "project_description.json"
    flash_args_path = build / "flasher_args.json"
    desc = json.loads(description_path.read_text())
    flashes = json.loads(flash_args_path.read_text())
    require(desc["project_name"] == "memory_native" and desc["target"] == "esp32s3", "Not ordinary S3 memory fixture build")
    require(pathlib.Path(desc["project_path"]).resolve() == FIXTURE.resolve(), "Build source path differs from authored fixture")
    require(pathlib.Path(desc["idf_path"]).resolve() == sdk, "Build did not use manifest SDK source")
    require(pathlib.Path(desc["build_dir"]).resolve() == build and build.is_relative_to(pathlib.Path(metadata["build_root"]).resolve()), "Build must use native SDK build root")
    elf, app = build / desc["app_elf"], build / desc["app_bin"]
    config = pathlib.Path(desc["config_file"])
    require(config.resolve().is_relative_to(build), "Build sdkconfig must be separate from fixture source")
    cfg = dict(re.findall(r"^(CONFIG_[A-Z0-9_]+)=(.*)$", config.read_text(), re.M))
    require(cfg.get("CONFIG_IDF_TARGET") == '"esp32s3"', "Explicit IDF target is not S3")
    if args.mode == "psram":
        require(all(cfg.get(k) == "y" for k in ("CONFIG_SPIRAM", "CONFIG_SPIRAM_MODE_QUAD", "CONFIG_SPIRAM_BOOT_INIT", "CONFIG_SPIRAM_USE_CAPS_ALLOC")), "PSRAM profile is not ordinary initialized quad capability heap")
        require(cfg.get("CONFIG_SPIRAM_IGNORE_NOTFOUND") != "y", "Missing PSRAM must never silently fall back")
    else:
        require(cfg.get("CONFIG_SPIRAM") != "y", "Internal leaf must not initialize PSRAM")
    image = args.flash.read_bytes()
    require(len(image) == 4 * 1024 * 1024, "Merged ordinary flash must have exact 4MiB extent")
    inputs = [args.qemu, args.flash, args.sdk_metadata, args.source_map, description_path, flash_args_path, elf, app, config, pathlib.Path(__file__).resolve()]
    for offset, filename in flashes["flash_files"].items():
        binary = build / filename
        data = binary.read_bytes()
        start = int(offset, 0)
        require(image[start:start + len(data)] == data, f"Merged flash differs from built input {filename}")
        inputs.append(binary)
    app_data = app.read_bytes()
    # Ordinary ESP image header (24B), first segment header (8B), app descriptor.
    require(struct.unpack_from("<I", app_data, 32)[0] == 0xabcd5432, "Missing ordinary IDF app descriptor")
    require(app_data[176:208].hex() == digest(elf), "App image does not embed the supplied ELF SHA256")
    for path in FIXTURE.rglob("*"):
        if path.is_file() and not any(part.startswith("build") or part == "__pycache__" for part in path.relative_to(FIXTURE).parts):
            inputs.append(path)
    # Record actual compiled SDK/fixture input sources, not merely version labels.
    sources = {}
    for component in desc["build_component_info"].values():
        for name in component.get("sources", []):
            path = pathlib.Path(name)
            if path.is_file():
                sources[str(path)] = digest(path)
    source_map = json.loads(args.source_map.read_text())
    require(source_map["base_commit"] == "40edccac415693c5130f91c01d84176ae6008566" and
            re.fullmatch(r"[0-9a-f]{64}", source_map.get("prefix_fingerprint") or "") and
            source_map.get("negative_without_spi1_fix") is False,
            "Source map does not describe the approved frozen positive candidate")
    (evidence / "source-map.json").write_text(json.dumps(source_map, indent=2) + "\n")
    # Bind the actual prepared lineage explicitly; its filename/location is not
    # an identity and need not come from one particular preparation frontend.
    prepared_path = args.prepared_source_record.resolve(strict=True)
    prepared = json.loads(prepared_path.read_text())
    require(prepared["base_commit"] == source_map["base_commit"] and
            prepared["fingerprint"] == source_map["prefix_fingerprint"] and
            pathlib.Path(prepared["source"]).resolve() == args.qemu_source,
            "QEMU standard source record is not this approved positive candidate")
    profile = prepared["profile"]
    profile_copies = {item["source"]: item["destination"] for item in profile["copies"]}
    profile_patches = {item["source"] for item in profile["patches"]}
    require(len(profile_copies) == len(profile["copies"]), "Profile copy sources are ambiguous")
    require("prototypes/memory/spi1-transport.patch" in profile_patches,
            "Positive candidate must contain the SPI1 transport index-bounds fix")
    for pinned in source_map["preparation_order"]:
        require(prepared["inputs"].get(pinned["source"]) == pinned["sha256"],
                f"Prepared input differs from pinned source map: {pinned['source']}")
        input_path = pathlib.Path(__file__).resolve().parents[3] / "qemu-extensions" / pinned["source"]
        require(digest(input_path) == prepared["inputs"][pinned["source"]],
                f"Current memory overlay input changed: {pinned['source']}")
        inputs.append(input_path)
        if pinned["kind"] == "copy":
            require(profile_copies.get(pinned["source"]) == pinned["destination"],
                    f"Profile copy destination differs from map: {pinned['source']}")
            require(digest(args.qemu_source / pinned["destination"]) == pinned["sha256"],
                    f"Prepared QEMU copy differs from map: {pinned['destination']}")
        else:
            require(pinned["source"] in profile_patches,
                    f"Profile is missing pinned memory patch: {pinned['source']}")
    for target, expected in source_map["applied_target_sha256"].items():
        require(prepared["applied_files"].get(target) == expected and
                digest(args.qemu_source / target) == expected,
                f"Prepared memory target changed: {target}")
    inputs.append(prepared_path)
    (evidence / "prepared-source.json").write_text(json.dumps(prepared, indent=2) + "\n")
    qemu_source = source_identity(args.qemu_source)
    require(qemu_source["commit"] == prepared["base_commit"], "Prepared QEMU HEAD is not the frozen base")
    (evidence / "qemu-source-identity.json").write_text(json.dumps(qemu_source, indent=2) + "\n")
    (evidence / "firmware-source-identity.json").write_text(json.dumps(sources, indent=2) + "\n")
    return dict(sdk=metadata, firmware_build=desc, configuration=cfg, source_map=source_map,
                artifacts={str(p): digest(p) for p in inputs}, compiled_sources=sources,
                qemu_source_commit=qemu_source["commit"], qemu_source_identity_sha256=digest(evidence / "qemu-source-identity.json"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("qemu", "qemu-source", "flash", "build-dir", "sdk-metadata", "source-map", "prepared-source-record", "evidence"):
        parser.add_argument(f"--{name}", required=True, type=pathlib.Path)
    parser.add_argument("--mode", required=True, choices=("internal", "psram"))
    parser.add_argument("--idf-profile", required=True, choices=LOCKS)
    parser.add_argument("--watchdog-seconds", type=float, default=120)
    args = parser.parse_args()
    if not 1 <= args.watchdog_seconds <= 600:
        parser.error("Host watchdog must be between 1 and 600 seconds")
    args.evidence.mkdir(parents=True, exist_ok=True)
    LIVE_ROOT.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f"{args.idf_profile}-{args.mode}-{time.strftime('%Y%m%dT%H%M%S')}-", dir=args.evidence.resolve()))
    result = dict(status="FAIL", mode=args.mode, idf_profile=args.idf_profile, evidence=str(evidence),
                  qualification="flash/raw/NVS reset and relaunch plus " + ("internal-only" if args.mode == "internal" else
                  "quad8MiB CPU cache/GDMA leaf; no remap/capacity-end/PSRAM-backing-reset/CPU-unmapped/octal qualification"),
                  commands=[], boots=[], disk={}, host_watchdog_seconds=args.watchdog_seconds)
    started = time.monotonic()
    proc = qmp = None
    original_hash = None
    live_sessions = []
    # QEMU writes the writable flash through this file for the whole run, so it
    # must live on a native disk (the evidence directory may be on drvfs, where
    # concurrent-append coherency is unreliable) and be published after QEMU exits.
    live_run = pathlib.Path(tempfile.mkdtemp(prefix="memory-run-", dir=LIVE_ROOT))
    scratch = live_run / "writable-flash.bin"
    try:
        for name in ("qemu", "qemu_source", "flash", "build_dir", "sdk_metadata", "source_map"):
            setattr(args, name, getattr(args, name).resolve(strict=True))
        original_hash = digest(args.flash)
        result["original_flash_before_sha256"] = original_hash
        result["provenance"] = provenance(args, evidence)
        image = args.flash.read_bytes()
        parts = flash_partitions(image)
        shutil.copyfile(args.flash, scratch)
        scratch.chmod(0o600)
        result["disk"]["fresh"] = disk_evidence(scratch, parts)
        require(result["disk"]["fresh"]["sha256"] == original_hash, "Fresh copy differs from immutable merged image")
        for session, phases in ((1, (1, 2)), (2, (3,))):
            # QEMU appends the guest log while the run is in flight; the evidence
            # directory may live on a drvfs mount, whose concurrent-write
            # coherency is not usable inside a polling loop (ENODATA or a
            # partially visible tail). Keep the live artifacts on native disk and
            # publish byte copies into the evidence directory once the session is
            # closed.
            live = pathlib.Path(tempfile.mkdtemp(prefix=f"memory-session{session}-", dir=LIVE_ROOT))
            live_sessions.append((session, live))
            uart = live / f"session{session}-uart.log"
            stdout, stderr = live / f"session{session}-stdout.log", live / f"session{session}-stderr.log"
            with socket.socket() as reserve:
                reserve.bind(("127.0.0.1", 0))
                port = reserve.getsockname()[1]
            command = [str(args.qemu), "-machine", "esp32s3", "-nographic", "-monitor", "none",
                       "-qmp", f"tcp:127.0.0.1:{port},server=on,wait=off",
                       "-m", "8M" if args.mode == "psram" else "0M", "-serial", f"file:{uart}",
                       "-drive", f"file={scratch},if=mtd,format=raw,cache=writethrough"]
            result["commands"].append(command)
            (evidence / "commands.json").write_text(json.dumps(result["commands"], indent=2) + "\n")
            with stdout.open("wb") as out, stderr.open("wb") as err:
                proc = subprocess.Popen(command, stdout=out, stderr=err)
                qmp = Qmp(port, proc, evidence / f"session{session}-qmp.jsonl", time.monotonic() + args.watchdog_seconds)
                offset = 0
                for phase in phases:
                    text = wait_boot(proc, uart, offset, args.watchdog_seconds)
                    (evidence / f"boot{phase}-uart.log").write_text(text)
                    consumer = validate_uart(text, args.mode, args.idf_profile)
                    persistence = validate_persistence(text, image, phase, parts)
                    result["boots"].append(dict(phase=phase, consumers=consumer, persistence=persistence))
                    result["disk"][f"boot{phase}"] = disk_evidence(scratch, parts, phase)
                    if phase == 1:
                        offset = len(read_guest_log(uart))
                        qmp.reset(args.watchdog_seconds)
                try:
                    qmp.command("quit")
                except (OSError, AssertionError):
                    # QEMU closes the QMP socket while shutting down; the quit
                    # response can be lost, and the process wait below is the
                    # real liveness check.
                    pass
                proc.wait(10)
                require(proc.returncode == 0, f"QEMU failed during orderly termination: {proc.returncode}")
                result.setdefault("qemu_returncodes", []).append(proc.returncode)
                qmp.close()
                qmp = None
                proc = None
            for name in ("uart.log", "stdout.log", "stderr.log"):
                source = live / f"session{session}-{name}"
                (evidence / f"session{session}-{name}").write_bytes(source.read_bytes() if source.exists() else b"")
            shutil.rmtree(live, ignore_errors=True)
            final_phase = phases[-1]
            result["disk"][f"session{session}_closed"] = disk_evidence(scratch, parts, final_phase)
        snapshots = [result["disk"][name] for name in ("fresh", "boot1", "boot2", "boot3")]
        require(len({row["sha256"] for row in snapshots}) == 4, "Flash disk did not change each committed phase")
        require(len({row["partition_sha256"]["nvs"] for row in snapshots}) == 4, "NVS partition did not change each committed phase")
        require(len({row["partition_sha256"]["factory"] for row in snapshots}) == 1, "App partition changed")
        result["status"] = "PASS"
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        if qmp is not None:
            qmp.close()
        if proc is not None:
            if proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.wait()
            result["qemu_returncode_after_cleanup"] = proc.returncode
        # Publish whatever the last live session produced, also on failure, so a
        # diagnostic run keeps its guest log and process streams.
        for session, live in live_sessions:
            for name in ("uart.log", "stdout.log", "stderr.log"):
                source = live / f"session{session}-{name}"
                target = evidence / f"session{session}-{name}"
                if source.exists() and not target.exists():
                    target.write_bytes(source.read_bytes())
        if scratch.exists():
            result["writable_flash_final_sha256"] = digest(scratch)
            shutil.copyfile(scratch, evidence / "writable-flash.bin")
            shutil.rmtree(live_run, ignore_errors=True)
        if original_hash is not None:
            result["original_flash_after_sha256"] = digest(args.flash)
            result["original_flash_unchanged"] = result["original_flash_after_sha256"] == original_hash
            if not result["original_flash_unchanged"]:
                result["status"] = "FAIL"
                result["error"] = "Original immutable merged flash changed"
        result["host_elapsed_seconds"] = time.monotonic() - started
        result["evidence_sha256"] = {p.name: digest(p) for p in evidence.iterdir() if p.is_file()}
        (evidence / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(status=result["status"], evidence=str(evidence), error=result.get("error"))))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
