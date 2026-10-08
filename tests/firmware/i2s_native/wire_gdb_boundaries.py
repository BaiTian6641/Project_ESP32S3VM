"""GDB-only read-only observer for the pinned ordinary I2S firmware ELF.

Stops on actual guest source-line boundaries; records exclusive RX file offsets
and live MMIO configuration while CPUs are stopped. No guest memory/register
writes, fabricated samples, injected clocks, or altered firmware are permitted.
Invoked by wire_verify.py, not as a standalone host module.
"""
import json
import os
from pathlib import Path
import re
import struct
import gdb

EVIDENCE = Path(os.environ["I2S_WIRE_EVIDENCE"])
OUTPUT = (EVIDENCE / "boundaries.jsonl").open("x", buffering=1)
SEEN = set()
PLAN = json.loads((EVIDENCE / "boundary-plan.json").read_text())
SOURCE_NAME = Path(PLAN["source"]).name


def event_counts():
    counts = []
    for controller in (0, 1):
        path = EVIDENCE / "rx" / f"i2s{controller}.rx.bin"
        size = path.stat().st_size
        if size < 16 or (size - 16) % 32:
            raise RuntimeError(f"not an exclusive complete-event boundary: {path} size={size}")
        counts.append((size - 16) // 32)
    return counts


class Boundary(gdb.Breakpoint):
    def __init__(self, line, phase, edge, raw_pdm):
        super().__init__(f"{SOURCE_NAME}:{line}", internal=True)
        self.phase, self.edge, self.line, self.raw_pdm = phase, edge, line, raw_pdm

    def stop(self):
        try:
            text = (EVIDENCE / "uart.log").read_text(errors="replace")
            rows = [dict(re.findall(r"([a-z_]+)=([^\s]+)", line))
                    for line in text.splitlines() if line.startswith("I2S_CASE ")]
            # UART drain is asynchronous. Attribute from the stopped guest's
            # actual case counter now; reconcile every CASE after console drain.
            row = rows[-1] if rows else None
            case = int(gdb.parse_and_eval("'i2s_native.c'::cases")) - 1
            key = (case, self.phase, self.edge)
            if key in SEEN:
                raise RuntimeError(f"duplicate attribution boundary {key}")
            SEEN.add(key)
            configs = []
            inferior = gdb.selected_inferior()
            for base in (0x6000f000, 0x6002d000):
                raw = bytes(inferior.read_memory(base + 0x20, 0x58))
                configs.append(dict(base=hex(base), first_offset="0x20",
                                    words=list(struct.unpack("<22I", raw))))
            OUTPUT.write(json.dumps(dict(case=case, phase=self.phase, raw_pdm=self.raw_pdm,
                                         edge=self.edge, source_line=self.line,
                                         pc=str(gdb.parse_and_eval("$pc")), serial_case_observed=row,
                                         event_counts=event_counts(), actual_i2s_configuration=configs)) + "\n")
            return False
        except Exception as exc:
            OUTPUT.write(json.dumps(dict(error=repr(exc), source_line=self.line)) + "\n")
            return True


class Complete(gdb.Breakpoint):
    def stop(self):
        OUTPUT.write(json.dumps(dict(complete=True, event_counts=event_counts())) + "\n")
        OUTPUT.close()
        return True




locations = []
for specification in PLAN["boundaries"]:
    boundary = Boundary(**specification)
    if len(boundary.locations) != 1:
        raise RuntimeError(f"actual ELF boundary has {len(boundary.locations)} locations: {specification}")
    locations.append(dict(specification, address=boundary.locations[0].address))
    gdb.write(f"ATTRIBUTION_BREAKPOINT {locations[-1]}\n")
completion = Complete(f"{SOURCE_NAME}:{PLAN['complete_line']}", internal=True)
if len(completion.locations) != 1:
    raise RuntimeError("actual ELF completion breakpoint must resolve uniquely")
complete_address = completion.locations[0].address
(EVIDENCE / "elf-boundary-addresses.json").write_text(json.dumps(
    dict(boundaries=locations, complete_address=complete_address), indent=2) + "\n")
gdb.execute("continue")
