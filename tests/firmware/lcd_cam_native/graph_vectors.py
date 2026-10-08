"""Public native-net v3 graphs, not device injection or test PASS assertions.

Each graph owns GPIOs exclusively; LCD and camera are never combined. UART0
GPIO43/44 and USB GPIO19/20 are not repurposed. GPIO26..38 are excluded for
flash/real PSRAM modules. All rails and passive pulls are explicit.
"""
import argparse
import json


def terminal(cid, role, domain="digital", gpio=None):
    result = dict(id=f"{cid}.{role}", name=role, role=role,
                  domain=domain, direction="unspecified")
    if gpio is not None:
        result["gpio"] = gpio
    return result


def component(cid, kind, terminals, parameters=None):
    return dict(id=cid, name=cid, type=kind, kind=kind,
                terminals=terminals, parameters=parameters or {})


DATA = (4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21)
RGB_FORMATS = {"serial8-rgb332": (8, 8), "serial8-rgb565": (8, 16),
               "serial8-rgb888": (8, 24), "parallel16-rgb888": (16, 24)}
RGB_SCENARIOS = tuple(f"rgb-{mode}-{fmt}" for mode in ("double", "bounce", "demand")
                      for fmt in RGB_FORMATS)
SCENARIOS = ("i80", "rgb-double", "rgb-bounce", "rgb-demand", *RGB_SCENARIOS,
             "camera-rgb565", "camera-yuv422", "camera-jpeg", "camera-multi",
             "camera-slow", "camera-truncation", "camera-overflow", "camera-reset",
             "camera-disconnected-sccb", "camera-disconnected-pclk")


def project(scenario="i80"):
    if scenario not in SCENARIOS:
        raise ValueError(f"unknown scenario {scenario}")
    camera = scenario.startswith("camera-")
    rgb = scenario.startswith("rgb-")
    if camera:
        kind = "ov2640-dvp"
        mapping = dict(sda=1, scl=2, **{f"d{i}": DATA[i] for i in range(8)},
                       pclk=12, reset=13, pwdn=14, xclk=15, vsync=16, href=17)
        params = dict(source="color-bars", timing_profile="ov2640-functional-dvp-v1")
    elif rgb:
        kind = "rgb-panel"
        bus_width, pixel_bits = next((value for name, value in RGB_FORMATS.items()
                                     if scenario.endswith(name)), (16, 16))
        mapping = dict(hsync=1, vsync=2, de=3, pclk=39,
                       **{f"d{i}": DATA[i] for i in range(bus_width)})
        params = dict(width=64, height=48, bus_width=bus_width, bits_per_pixel=pixel_bits,
                      pclk_active_high=True, de_active_high=True,
                      hsync_active_high=False, vsync_active_high=False,
                      hsync_pulse_width=2, h_back_porch=4, h_front_porch=4,
                      vsync_pulse_width=2, v_back_porch=2, v_front_porch=2)
    else:
        kind = "st7789-i80"
        mapping = dict(wr=1, dc=2, cs=3, reset=12,
                       **{f"d{i}": DATA[i] for i in range(8)})
        params = dict(width=64, height=48, bus_width=8)
    pins = sorted(set(mapping.values()))
    parts = [component("U1", "mcu", [terminal("U1", "vdd", "power"),
             terminal("U1", "gnd", "ground")] +
             [terminal("U1", f"io{pin}", gpio=pin) for pin in pins]),
             component("V", "voltage-source", [terminal("V", "p", "power"),
             terminal("V", "n", "ground")], {"voltage": {"value": 3.3, "unit": "V"}}),
             component("G", "ground", [terminal("G", "ref", "ground")]),
             component("D", kind, [terminal("D", "vdd", "power"),
             terminal("D", "gnd", "ground")] + [terminal("D", r) for r in mapping], params)]
    nets = [dict(id="gnd", name="gnd", endpoints=["G.ref", "V.n", "U1.gnd", "D.gnd"]),
            dict(id="vdd", name="vdd", endpoints=["V.p", "U1.vdd", "D.vdd"])]
    # Registered service bodies follow existing native peer attribute
    # convention. Do not expose a second parameters-based service schema.
    parts[-1]["kind"] = "device"
    parts[-1]["parameters"] = {}
    parts[-1]["attributes"] = {
        "native_camera" if camera else "native_lcd_panel": params}
    if rgb:
        # Official RGB panel has no reset GPIO. Real external reset is held
        # deasserted by a passive resistor, not an invented firmware pin.
        parts[-1]["terminals"].append(terminal("D", "reset"))
        parts.append(component("ResetPull", "resistor", [terminal("ResetPull", "a", "passive"),
            terminal("ResetPull", "b", "passive")], {"resistance": {"value": 10000, "unit": "ohm"}}))
        nets[1]["endpoints"].append("ResetPull.b")
        nets.append(dict(id="reset", name="reset", endpoints=["D.reset", "ResetPull.a"]))
    for role, pin in mapping.items():
        endpoints = [f"U1.io{pin}", f"D.{role}"]
        disconnected = (scenario == "camera-disconnected-sccb" and role == "sda") or (
            scenario == "camera-disconnected-pclk" and role == "pclk")
        if disconnected:
            nets.append(dict(id=f"mcu-{role}", name=f"mcu-{role}", endpoints=[endpoints.pop(0)]))
        nets.append(dict(id=role, name=role, endpoints=endpoints))
        # SCCB open drain pull-ups, active-low reset/select pull-ups, all
        # other digital wires explicitly idle-low. Pulls never synthesize clocks.
        high = role in ("sda", "scl", "reset", "cs")
        pull = f"Pull-{role}"
        parts.append(component(pull, "resistor", [terminal(pull, "a", "passive"),
            terminal(pull, "b", "passive")], {"resistance": {"value": 4700 if role in ("sda", "scl") else 10000, "unit": "ohm"}}))
        nets[-1]["endpoints"].append(f"{pull}.a")
        nets[1 if high else 0]["endpoints"].append(f"{pull}.b")
        if disconnected:
            # The disconnected MCU input/open-drain side also has a real pull.
            pull2 = f"McuPull-{role}"
            parts.append(component(pull2, "resistor", [terminal(pull2, "a", "passive"),
                terminal(pull2, "b", "passive")], {"resistance": {"value": 10000, "unit": "ohm"}}))
            nets[-2]["endpoints"].append(f"{pull2}.a")
            nets[1 if high else 0]["endpoints"].append(f"{pull2}.b")
    return dict(version=3, id=f"lcd-cam-native-{scenario}", name=f"LCD CAM native {scenario}",
        profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
        firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
        components=parts, nets=nets, geometry=dict(components={}, nets={}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scenario", choices=SCENARIOS)
    args = parser.parse_args()
    print(json.dumps(project(args.scenario), indent=2))
