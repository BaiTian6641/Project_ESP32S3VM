"""Version-3 registered i2s-sample-peer cases for the ordinary IDF fixture.

CLI: python graphs.py --case philips-16-mcu-master-0 --output project.json
The source array is materialized in JSON: the native peer never generates it.
"""
import argparse
import json
from pathlib import Path

FRAMES = 96
RATE = 12500


def sample(source, frame, slot, bits):
    value = ((frame * 73 + slot * 29 + source * 101 + 17) ^
             ((frame + slot + 1) * 0x9E3779B9)) & 0xFFFFFFFF
    return value & ((1 << bits) - 1)


def samples(source, bits, mask):
    return [sample(source, frame, slot, bits) for frame in range(FRAMES)
            for slot in range(16) if mask & (1 << slot)]


def raw_bits(source):
    # S3 raw codec: physical CLK low half is left, high half right;
    # each channel shifts its 16-bit DMA word MSB first.
    return [sample(source, frame, slot, 16) >> bit & 1
            for frame in range(FRAMES) for bit in range(15, -1, -1)
            for slot in (0, 1)]


def cases():
    result = {}
    for controller in (0, 1):
        for master in (True, False):
            role = "master" if master else "slave"
            for fmt in ("philips", "msb", "pcm"):
                for bits in (8, 16, 24, 32):
                    name = f"{fmt}-{bits}-mcu-{role}-{controller}"
                    result[name] = dict(format=fmt, bits=bits, slots=2, mask=3,
                                        master=master, controller=controller)
            for bits, slots, mask in ((8, 16, 0x8181), (16, 8, 0xA5),
                                       (24, 4, 0xB), (32, 4, 0xD)):
                name = f"tdm-{bits}-s{slots}-m{mask:x}-mcu-{role}-{controller}"
                result[name] = dict(format="tdm", bits=bits, slots=slots, mask=mask,
                                    master=master, controller=controller)
            name = f"raw-pdm-16-mcu-{role}-{controller}"
            result[name] = dict(format="raw-pdm", bits=16, slots=2, mask=3,
                                master=master, controller=controller)
    return result


def terminal(cid, role, domain, gpio=None, direction="unspecified"):
    value = dict(id=f"{cid}.{role}", name=role, role=role, domain=domain,
                 direction=direction)
    if gpio is not None:
        value["gpio"] = gpio
    return value


def component(cid, kind, terms, parameters=None):
    return dict(id=cid, name=cid, type=kind,
                kind="device" if kind == "i2s-sample-peer" else kind, terminals=terms,
                parameters={} if parameters is None else parameters)


def parameters(case):
    fmt, bits = case["format"], case["bits"]
    raw = fmt == "raw-pdm"
    value = dict(role="slave" if case["master"] else "master", format=fmt,
                 dataBits=bits, slotBits=bits, slots=case["slots"],
                 slotMask=case["mask"], wsWidth=1 if fmt in ("pcm", "tdm", "raw-pdm") else bits,
                 wsPol=fmt in ("pcm", "tdm"), bitShift=fmt in ("philips", "pcm", "tdm"),
                 leftAlign=fmt in ("philips", "msb", "pcm"), lsbFirst=False,
                 sampleRateNumeratorHz=62500 if raw else RATE,
                 sampleRateDenominator=1, repeat=True, captureCapacity=65536)
    value["rawBits" if raw else "txSamples"] = raw_bits(1) if raw else samples(1, bits, case["mask"])
    return value

def project(name, stage="peer"):
    case = cases()[name]
    raw = case["format"] == "raw-pdm"
    components = [
        component("U1", "mcu", [terminal("U1", "vdd", "power"),
                  terminal("U1", "gnd", "ground")] +
                  [terminal("U1", f"io{pin}", "digital", pin) for pin in (18, 19, 20, 21)]),
        component("Peer", "i2s-sample-peer", [terminal("Peer", "vdd", "power", direction="input"),
                  terminal("Peer", "gnd", "ground", direction="input")] +
                  [terminal("Peer", role, "digital", direction="output" if
                      role == "dout" or (role in ("bclk", "ws") and not case["master"])
                      else "input") for role in ("bclk", "ws", "din", "dout")]),
        component("G", "ground", [terminal("G", "ref", "ground")]),
        component("V", "voltage-source", [terminal("V", "p", "power"),
                  terminal("V", "n", "ground")], {"voltage": {"value": 3.3, "unit": "V"}}),
    ]
    components[1]["attributes"] = {"native_i2s_peer": parameters(case)}
    nets = [dict(id="gnd", name="gnd", endpoints=["G.ref", "V.n", "U1.gnd", "Peer.gnd"]),
            dict(id="vdd", name="vdd", endpoints=["V.p", "U1.vdd", "Peer.vdd"]),
            dict(id="bclk", name="bclk", endpoints=["Peer.bclk"] if raw else ["U1.io18", "Peer.bclk"]),
            dict(id="ws", name="ws", endpoints=["U1.io18" if raw else "U1.io19", "Peer.ws"]),
            dict(id="mcu-tx", name="mcu-tx", endpoints=["U1.io20", "Peer.din"]),
            dict(id="peer-tx", name="peer-tx", endpoints=["Peer.dout", "U1.io21"])]
    # Explicit clock/data idle bias lets resolved frames exist before the
    # ordinary firmware enables its driver, without fabricating a source.
    for net in nets[2:]:
        cid = "Idle" + net["id"]
        components.append(component(cid, "resistor", [terminal(cid, "a", "passive"),
                          terminal(cid, "b", "passive")],
                          {"resistance": {"value": 10000, "unit": "ohm"}}))
        net["endpoints"].append(f"{cid}.a")
        nets[0]["endpoints"].append(f"{cid}.b")
    if stage == "boot":
        components = [item for item in components if item["id"] != "Peer"]
        for net in nets:
            net["endpoints"] = [endpoint for endpoint in net["endpoints"]
                                if not endpoint.startswith("Peer.")]
    elif stage != "peer":
        raise ValueError(f"unknown launch stage {stage}")
    return dict(version=3, id=f"i2s-peer-{name}", name=f"I2S peer {name}",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
                firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
                components=components, nets=nets, geometry=dict(components={}, nets={}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--case", choices=sorted(cases()))
    parser.add_argument("--output", type=Path)
    parser.add_argument("--stage", choices=("boot", "peer"), default="peer")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    if args.list:
        print("\n".join(cases()))
        return
    if args.case is None or args.output is None:
        parser.error("--case and --output are required unless --list")
    args.output.write_text(json.dumps(project(args.case, args.stage), indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
