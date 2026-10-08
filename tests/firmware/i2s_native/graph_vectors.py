"""Version-3 native electrical projects; public QMP Apply only."""

def terminal(component, role, domain, gpio=None):
    result = dict(id=f"{component}.{role}", name=role, role=role,
                  domain=domain, direction="unspecified")
    if gpio is not None:
        result["gpio"] = gpio
    return result


def component(cid, kind, terminals, parameters=None):
    return dict(id=cid, name=cid, type=kind, kind=kind,
                terminals=terminals, parameters=parameters or {})


def project(scenario="connected"):
    if scenario == "external-peer":
        raise ValueError("UNQUALIFIED: registered external I2S peer schema/seam not supplied; GPIO cross-controller coverage is not external-peer coverage")
    if scenario not in ("connected", "disconnected-data"):
        raise ValueError(f"unknown scenario {scenario}")
    terms = [terminal("U1", "vdd", "power"), terminal("U1", "gnd", "ground")]
    terms += [terminal("U1", f"io{pin}", "digital", pin) for pin in (18, 19, 20, 21, 4, 5, 6, 7)]
    components = [component("U1", "mcu", terms),
                  component("G", "ground", [terminal("G", "ref", "ground")]),
                  component("V", "voltage-source", [terminal("V", "p", "power"),
                      terminal("V", "n", "ground")], {"voltage": {"value": 3.3, "unit": "V"}})]
    nets = [dict(id="gnd", name="gnd", endpoints=["G.ref", "V.n", "U1.gnd"]),
            dict(id="vdd", name="vdd", endpoints=["V.p", "U1.vdd"]),
            dict(id="bclk", name="bclk", endpoints=["U1.io18", "U1.io4"]),
            dict(id="ws", name="ws", endpoints=["U1.io19", "U1.io5"])]
    # Explicit idle-low clock resolution. WS low is the deasserted PCM-short
    # pulse level; active master drives either polarity during every frame.
    for index, name in ((2, "bclk"), (3, "ws")):
        cid = f"Idle{name}"
        components.append(component(cid, "resistor", [terminal(cid, "a", "passive"),
            terminal(cid, "b", "passive")], {"resistance": {"value": 10000, "unit": "ohm"}}))
        nets[0]["endpoints"].append(f"{cid}.b")
        nets[index]["endpoints"].append(f"{cid}.a")
    for source, destination in ((20, 7), (6, 21)):
        if scenario == "connected":
            nets.append(dict(id=f"data{source}", name=f"data{source}",
                             endpoints=[f"U1.io{source}", f"U1.io{destination}"]))
        else:
            cid = f"Pull{destination}"
            components.append(component(cid, "resistor", [terminal(cid, "a", "passive"),
                terminal(cid, "b", "passive")], {"resistance": {"value": 10000, "unit": "ohm"}}))
            nets[0]["endpoints"].append(f"{cid}.b")
            nets.extend([dict(id=f"tx{source}", name=f"tx{source}", endpoints=[f"U1.io{source}"]),
                         dict(id=f"rx{destination}", name=f"rx{destination}",
                              endpoints=[f"U1.io{destination}", f"{cid}.a"])])
    return dict(version=3, id=f"i2s-native-{scenario}", name=f"I2S native {scenario}",
                profile=dict(chip="esp32s3", board="esp32-s3-devkitc-1", module="esp32-s3-wroom-1"),
                firmware={}, runtime=dict(electrical=dict(driver_profile="s3-explicit-finite-v1", mode="dc")),
                components=components, nets=nets, geometry=dict(components={}, nets={}))
