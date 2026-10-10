#!/usr/bin/env python3
"""Qualify ordinary IDF SPI2/3 against the native v3 electrical graph.

Run under WSL/Linux. No register writes, clock advances, firmware patches,
provider substitutes or favorable incoming data. qtest is read-only evidence.
The wall-clock watchdog diagnoses a hung host; it is not a guest timeout.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
IDF_COMMIT = 'fff9895c82d744c7237be8847347bdd1b07c6643'
PROFILES = {'connected': 'CONNECTED', 'loopback': 'LOOPBACK',
            'wrong_cs': 'WRONG_CS', 'disconnected_miso': 'DISCONNECTED_MISO',
            'owner_error': 'OWNER_ERROR'}
PINS = {2: (12, 11, 13, 10, 9), 3: (36, 35, 37, 34, 33)}
LARGE_BYTES = 8201


def terminal(cid, name, domain='digital', direction='inout', gpio=None):
    value = dict(id=f'{cid}.{name}', name=name, role='gpio' if gpio is not None else name,
                 domain=domain, direction=direction)
    if gpio is not None:
        value['gpio'] = gpio
    return value


def component(cid, kind, terminals, parameters=None, model=None):
    return dict(id=cid, name=cid, kind=kind, type=model or kind,
                terminals=terminals, parameters=parameters or {})


def loopback_graph():
    components = [
        component('U1', 'mcu', [terminal('U1', 'vdd', 'power', 'input'),
                  terminal('U1', 'gnd', 'ground', 'input')] +
                  [terminal('U1', f'io{pad}', gpio=pad) for pad in sorted({p for row in PINS.values() for p in row})]),
        component('G', 'ground', [terminal('G', 'ref', 'ground', 'passive')]),
        component('V', 'voltage-source', [terminal('V', 'p', 'power', 'output'),
                  terminal('V', 'n', 'ground', 'input')], {'voltage': {'value': 3.3, 'unit': 'V'}}),
    ]
    endpoints = {'gnd': ['G.ref', 'V.n', 'U1.gnd'], 'vdd': ['V.p', 'U1.vdd']}
    for host, (clock, mosi, miso, cs0, cs1) in PINS.items():
        endpoints[f'spi{host}_loop'] = [f'U1.io{mosi}', f'U1.io{miso}']
        for name, pad in (('sclk', clock), ('cs0', cs0), ('cs1', cs1)):
            endpoints[f'spi{host}_{name}'] = [f'U1.io{pad}']
    return dict(version=3, id='spi-native-loopback', name='Native SPI physical loopback',
                profile=dict(chip='esp32s3', board='spi-native-simulation-fixture',
                             module='bare-esp32s3-simulation-fixture', reserved_gpios=[], reservations_known=False),
                firmware={}, runtime=dict(electrical=dict(driver_profile='s3-explicit-finite-v1', mode='dc')),
                components=components, nets=[dict(id=name, name=name, endpoints=ends) for name, ends in endpoints.items()],
                geometry=dict(components={}, nets={}))
def project(mode, negative_host):
    graph = loopback_graph()
    graph['id'] = f'spi-native-{mode}'
    graph['name'] = f'Native SPI ordinary driver {mode}'
    if mode in ('loopback', 'owner_error'):
        return graph
    components = graph['components']
    endpoints = {net['id']: net['endpoints'] for net in graph['nets']}
    for host, (_, mosi, miso, _, _) in PINS.items():
        del endpoints[f'spi{host}_loop']
        endpoints[f'spi{host}_mosi'] = [f'U1.io{mosi}']
        endpoints[f'spi{host}_miso'] = [f'U1.io{miso}']
        for cs in range(2):
            cid = f'NOR{host}_{cs}'
            components.append(component(cid, 'device', [
                terminal(cid, 'mosi', direction='input'), terminal(cid, 'miso', direction='output'),
                terminal(cid, 'sclk', direction='input'), terminal(cid, 'cs', direction='input'),
                terminal(cid, 'vdd', 'power', 'input'), terminal(cid, 'gnd', 'ground', 'input'),
            ], {}, 'spi-nor-1m'))
            endpoints['vdd'].append(f'{cid}.vdd')
            endpoints['gnd'].append(f'{cid}.gnd')
            endpoints[f'spi{host}_mosi'].append(f'{cid}.mosi')
            endpoints[f'spi{host}_sclk'].append(f'{cid}.sclk')
            miso_net = f'spi{host}_miso'
            if mode == 'disconnected_miso' and host == negative_host:
                miso_net = f'spi{host}_device_miso'
                endpoints.setdefault(miso_net, [])
            endpoints[miso_net].append(f'{cid}.miso')
            cs_net = f'spi{host}_cs{cs}'
            if mode == 'wrong_cs' and host == negative_host and cs == 0:
                wrong_pad = 14 if host == 2 else 38
                components[0]['terminals'].append(terminal('U1', f'io{wrong_pad}', gpio=wrong_pad))
                cs_net = f'spi{host}_wrong_cs'
                endpoints[cs_net] = [f'U1.io{wrong_pad}']
            endpoints[cs_net].append(f'{cid}.cs')
            # Real external CS pull-up keeps a never-configured/wrong CS
            # inactive. There is deliberately NO master MISO pull-up.
            resistor = f'RCS{host}_{cs}'
            components.append(component(resistor, 'resistor', [
                terminal(resistor, 'a', 'passive', 'passive'),
                terminal(resistor, 'b', 'passive', 'passive')],
                {'resistance': {'value': 10000, 'unit': 'ohm'}}))
            endpoints[cs_net].append(f'{resistor}.a')
            endpoints['vdd'].append(f'{resistor}.b')
    graph['nets'] = [dict(id=name, name=name, endpoints=ends) for name, ends in endpoints.items()]
    return graph




def require(condition, message):
    if not condition:
        raise AssertionError(message)


def configuration(path, mode):
    values = dict(re.findall(r'^(CONFIG_[A-Z0-9_]+)=(.*)$', path.read_text(), re.MULTILINE))
    selected = [key.removeprefix('CONFIG_SPI_NATIVE_PROFILE_') for key, value in values.items()
                if key.startswith('CONFIG_SPI_NATIVE_PROFILE_') and value == 'y']
    require(selected == [PROFILES[mode]], f'Graph {mode} requires firmware profile {PROFILES[mode]}, selected {selected}')
    require(values.get('CONFIG_IDF_TARGET') == '"esp32s3"', 'Fixture target must be esp32s3')
    speed = int(values['CONFIG_SPI_NATIVE_SPEED_HZ'])
    require(100000 <= speed <= 1000000, 'Fixture SCLK outside qualification range')
    if mode == 'owner_error':
        require(values.get('CONFIG_SPI_NATIVE_NEGATIVE_HOST') == '3', 'Owner-error fixture targets SPI3 only')
    return dict(profile=mode, speed_hz=speed, pins=PINS,
                negative_host=int(values.get('CONFIG_SPI_NATIVE_NEGATIVE_HOST', '2')))


class Qmp:
    def __init__(self, path, transcript, deadline):
        self.deadline, self.transcript, self.sequence = deadline, transcript, 0
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(min(10, max(.001, deadline - time.monotonic())))
        self.sock.connect(str(path))
        self.stream = self.sock.makefile('rwb', buffering=0)
        require('QMP' in self.receive(), 'Invalid QMP greeting')
        self.call('qmp_capabilities')

    def receive(self):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError('Host diagnostic watchdog expired during QMP')
        self.sock.settimeout(min(10, remaining))
        line = self.stream.readline()
        if not line:
            raise RuntimeError('QMP closed before response')
        response = json.loads(line)
        self.transcript.append(dict(direction='receive', message=response))
        return response

    def call(self, command, arguments=None):
        self.sequence += 1
        request = dict(execute=command, arguments=arguments or {}, id=self.sequence)
        self.transcript.append(dict(direction='send', message=request))
        self.stream.write((json.dumps(request) + '\n').encode())
        while True:
            response = self.receive()
            if 'event' in response:
                continue
            require(response.get('id') == request['id'], f'Unmatched QMP response: {response}')
            if 'error' in response:
                raise RuntimeError(response['error'])
            return response['return']

    def snapshot(self):
        return json.loads(self.call('qom-get', dict(path='/machine/soc/electrical', property='snapshot-json')))

    def close(self):
        self.stream.close()
        self.sock.close()


def read_registers(path, host, deadline, text, owner_error=False):
    """No writes or clock_step: inspect the paused actual controller only."""
    base = 0x60024000 if host == 2 else 0x60025000
    addresses = {'command': base, 'interrupt_raw': base + 0x3c, 'interrupt_masked': base + 0x40}
    if owner_error:
        addresses.update(rx_raw=0x6003f008, rx_eof=0x6003f028)
        match = re.search(r'^SPI_NATIVE_OWNER_ARM d0=([0-9a-f]+) d1=([0-9a-f]+) buffer=([0-9a-f]+)$', text, re.MULTILINE)
        require(match, 'Missing real owner-error descriptor addresses')
        for name, value in zip(('d0', 'd1', 'buffer'), match.groups()):
            address = int(value, 16)
            require(0x3fc88000 <= address <= 0x3fd00000 - 12 and not address & 3,
                    f'Invalid guest DMA-capable address {name}={address:x}')
            for index in range(3 if name != 'buffer' else 2):
                addresses[f'{name}_{index}'] = address + index * 4
    registers = {}
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(min(10, max(.001, deadline - time.monotonic())))
        sock.connect(str(path))
        with sock.makefile('rwb', buffering=0) as stream:
            for name, address in addresses.items():
                request = f'readl 0x{address:x}'
                stream.write((request + '\n').encode())
                response = stream.readline().decode().strip()
                require(re.fullmatch(r'OK 0x[0-9a-fA-F]+', response), f'qtest {request}: {response}')
                registers[name] = int(response.split()[1], 16)
    return registers


def fnv1a(data):
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return f'{value:08x}'


def observations(text):
    return {name: re.findall(rf'^SPI_NATIVE_{name} .*$', text, re.MULTILINE)
            for name in ('BOOT', 'PHASE', 'CHECK', 'STATUS', 'RX', 'EVENT', 'DESCRIPTOR',
                         'NOR_STATUS', 'BUSY', 'BITS', 'CONTROL', 'DIRECT', 'NEGATIVE_ARM', 'GATE', 'DONE')}


def validate_uart(text, config):
    mode = config['profile']
    require(re.search(rf'^SPI_NATIVE_BOOT profile={mode}$', text, re.MULTILINE), 'Missing matching fixture boot')
    require(re.search(rf'^SPI_NATIVE_DONE profile={mode} failures=0 result=PASS virtual_us=\d+$', text, re.MULTILINE),
            'Ordinary driver fixture did not complete successfully')
    require(not re.search(r'^SPI_NATIVE_CHECK .*result=FAIL$', text, re.MULTILINE), 'Firmware reported a failed check')
    for host, row in PINS.items():
        for engine in ('PIO', 'GDMA'):
            require(re.search(rf'^SPI_NATIVE_PHASE host={host} engine={engine} sclk={row[0]} mosi={row[1]} miso={row[2]} cs0={row[3]} cs1={row[4]} speed_hz={config["speed_hz"]} route=matrix virtual_us=\d+$', text, re.MULTILINE),
                    f'SPI{host} {engine} did not run against the configured GPIO matrix pins')
        checks = dict(re.findall(rf'^SPI_NATIVE_CHECK host={host} name=(\w+) result=(\w+)$', text, re.MULTILINE))
        rows = {}
        for name, size, digest, raw in re.findall(rf'^SPI_NATIVE_RX host={host} name=(\w+) bytes=(\d+) fnv1a=([0-9a-f]{{8}}) data=([0-9a-f]+)$', text, re.MULTILINE):
            data = bytes.fromhex(raw)
            require(len(data) == int(size) and fnv1a(data) == digest, f'SPI{host} {name} payload/hash inconsistent')
            require(name not in rows, f'SPI{host} duplicate RX evidence {name}')
            rows[name] = data
        expected_checks = {'bus_init', 'add_device', 'remove_device', 'bus_free', 'dma_alloc'}
        expected_events = []
        if mode == 'connected':
            expected_checks |= {'jedec_id', 'initial_status', 'program_without_wren_ignored', 'wren_wel', 'program_busy',
                                'program_ready', 'two_cs_wel_isolation', 'program_read_exact', 'two_cs_storage_isolation',
                                'nor_one_to_zero_only', 'erase_busy', 'erase_ready', 'erase_exact', 'nor_dma_exact',
                                'nor_dma_two_cs', 'nor_result_identity', 'nor_dma_descriptors_irq', 'other_cs_dma_descriptors_irq'}
            expected_rows = {'jedec_cs0_mode0': bytes.fromhex('ef4014'), 'jedec_cs1_mode3': bytes.fromhex('ef4014'),
                             'program_read48': bytes((i * 37 + host * 11) & 255 for i in range(48)),
                             'other_cs48': b'\xff' * 48, 'erased48': b'\xff' * 48,
                             'no_wren48': b'\xff' * 48,
                             'one_to_zero48': bytes((i * 37 + host * 11) & 255 for i in range(48)),
                             'nor_dma8201': bytes(((i * 73) ^ (i >> 7) ^ (host * 31)) & 255 for i in range(LARGE_BYTES)),
                             'other_cs_dma8201': b'\xff' * LARGE_BYTES}
            for name, expected in expected_rows.items():
                require(rows.get(name) == expected, f'SPI{host} missing/incorrect actual NOR RX {name}')
            expected_events = [('nor_dma_descriptors_irq', ('RX',)), ('other_cs_dma_descriptors_irq', ('RX',))]
            busy = re.findall(rf'^SPI_NATIVE_BUSY host={host} name=(program_ready|erase_ready) polls=(\d+) elapsed_us=(\d+) first_status=([0-9a-f]+) final=00$', text, re.MULTILINE)
            require({name for name, *_ in busy} == {'program_ready', 'erase_ready'}, f'SPI{host} missing NOR busy/deadline results')
            floors = {'program_ready': 600, 'erase_ready': 40000}
            require(all(int(polls) >= 2 and int(elapsed) >= floors[name] and int(first, 16) & 1
                        for name, polls, elapsed, first in busy),
                    f'SPI{host} NOR busy deadline/poll evidence violates fixed model durations')
        else:
            expected_checks |= {'loopback_polling', 'nonbyte_phases_polling', 'controlled_warmup_exact',
                                'soft_reset_cancel', 'local_gate_cancel', 'system_gate_cancel', 'system_reset_cancel',
                                'controlled_recovery_exact', 'queue_order_identity', 'two_queued_transactions',
                                'queued0_descriptors_irq', 'queued1_descriptors_irq'}
            for spi_mode in range(4):
                for index, order in enumerate(('msb', 'lsb', 'txlsb_rxmsb', 'txmsb_rxlsb')):
                    name = f'loopback_mode{spi_mode}_{order}61'
                    tx = bytes((i * 29 + spi_mode * 17 + index * 71 + host) & 255 for i in range(61))
                    expected = bytes(int(f'{byte:08b}'[::-1], 2) for byte in tx) if index >= 2 else tx
                    require(rows.get(name) == expected, f'SPI{host} mode{spi_mode} {order} exact loopback mismatch')
                    bitname = f'nonbyte_mode{spi_mode}_{order}13'
                    data = rows.get(bitname, b'')
                    mask = 0x1f if index in (1, 3) else 0xf8
                    require(len(data) == 2 and data[0] == expected[0] and data[1] & mask == expected[1] & mask,
                            f'SPI{host} non-byte mode{spi_mode} {order} mismatch')
                    expected_checks |= {name, bitname}
                    phase = re.search(rf'^SPI_NATIVE_BITS host={host} mode={spi_mode} order={order} command_bits=5 address_bits=11 dummy_bits=3 data_bits=13 elapsed_us=(\d+) user=([0-9a-f]+) user1=([0-9a-f]+) user2=([0-9a-f]+) dlen=([0-9a-f]+)$', text, re.MULTILINE)
                    require(phase, f'SPI{host} missing actual command/address/dummy/nonbyte state')
                    elapsed, user, user1, user2, dlen = phase.groups()
                    require(int(user, 16) & 0xe0000000 == 0xe0000000
                            and (int(user1, 16) >> 27) & 31 == 10 and int(user1, 16) & 255 == 2
                            and (int(user2, 16) >> 28) & 15 == 4 and int(dlen, 16) & 0x3ffff == 12
                            and int(elapsed) >= 32 * 1000000 // config['speed_hz'],
                            f'SPI{host} requested phases did not reach actual registers/clock timing')
            for slot in range(2):
                name = f'loopback_dma8201_slot{slot}'
                expected = bytes(((i * 73) ^ (i >> 7) ^ (host * 31) ^ (slot * 0xa7)) & 255 for i in range(LARGE_BYTES))
                require(rows.get(name) == expected, f'SPI{host} queued GDMA slot{slot} exact RX mismatch')
                expected_checks.add(name)
                expected_events.append((f'queued{slot}_descriptors_irq', ('RX', 'TX')))
            expected_checks |= {'prefix_queue', 'prefix_result', 'full_duplex_rx_prefix_exact',
                                'full_duplex_rx_prefix_bounds', 'full_duplex_rx_prefix_irq'}
            prefix = bytes(((i * 73) ^ (i >> 7) ^ (host * 31)) & 255 for i in range(4))
            require(rows.get('full_duplex_rx_prefix4') == prefix, f'SPI{host} shorter full-duplex RX prefix mismatch')
            events = re.findall(rf'^SPI_NATIVE_EVENT host={host} name=full_duplex_rx_prefix_irq (.*)$', text, re.MULTILINE)
            require(len(events) == 1, f'SPI{host} missing shorter-RX callback event')
            event = dict(re.findall(r'(\w+)=(-?[0-9a-f]+)', events[0]))
            minimum_us = 8 * 8 * 1000000 // config['speed_hz']
            require(event['pre'] == event['post'] == '1'
                    and int(event['end_us']) - int(event['start_us']) >= minimum_us
                    and int(event['spi_raw'], 16) & (1 << 12), f'SPI{host} shorter-RX callback/IRQ/timing failure')
            desc = re.findall(rf'^SPI_NATIVE_DESCRIPTOR host={host} name=full_duplex_rx_prefix_irq direction=RX index=(\d+) address=([0-9a-f]+) before=([0-9a-f]+) after=([0-9a-f]+)$', text, re.MULTILINE)
            require(len(desc) == 1 and desc[0][0] == '0', f'SPI{host} shorter-RX descriptor chain mismatch')
            _, address, before, after = desc[0]
            require(int(before, 16) & (1 << 31) and int(after, 16) == 0x40004004
                    and int(event['rx_eof'], 16) == int(address, 16) and int(event['rx_raw'], 16) & 2,
                    f'SPI{host} shorter-RX exact length/owner/EOF failure')
            controls = re.findall(rf'^SPI_NATIVE_CONTROL host={host} name=(\w+) was_active=1 command=([0-9a-f]+) raw=([0-9a-f]+) elapsed_us=(\d+)$', text, re.MULTILINE)
            require({name for name, *_ in controls} == {'soft_reset_cancel', 'local_gate_cancel', 'system_gate_cancel', 'system_reset_cancel'},
                    f'SPI{host} missing live-transfer cancellation evidence')
            require(all(not int(command, 16) & (1 << 24) and not int(raw, 16) & (1 << 12) and int(elapsed) >= 60000
                        for _, command, raw, elapsed in controls), f'SPI{host} cancelled transaction completed late')
            expected_checks |= {'pll_xtal_start', 'pll_xtal_end', 'pll_cpu_xtal_exact'}
            expected = bytes((i * 29 + host) & 255 for i in range(61))
            require(rows.get('pll_cpu_xtal61') == expected, f'SPI{host} CPU-XTAL independent PLL transfer RX mismatch')
            pll = re.findall(rf'^SPI_NATIVE_PLL host={host} cpu_mux=0 pll_source=1 pll_powered=1 elapsed_us=(\d+)$', text, re.MULTILINE)
            require(len(pll) == 1 and int(pll[0]) >= 61 * 8 * 1000000 // config['speed_hz'],
                    f'SPI{host} missing actual CPU-XTAL/peripheral-PLL timing evidence')
        require(all(checks.get(name) == 'PASS' for name in expected_checks),
                f'SPI{host} missing passing checks: {sorted(name for name in expected_checks if checks.get(name) != "PASS")}')
        for name, directions in expected_events:
            events = re.findall(rf'^SPI_NATIVE_EVENT host={host} name={name} (.*)$', text, re.MULTILINE)
            require(len(events) == 1, f'SPI{host} missing unique callback event {name}')
            event = dict(re.findall(r'(\w+)=(-?[0-9a-f]+)', events[0]))
            minimum_us = LARGE_BYTES * 8 * 1000000 // config['speed_hz']
            require(event['pre'] == event['post'] == '1'
                    and int(event['end_us']) - int(event['start_us']) >= minimum_us
                    and int(event['spi_raw'], 16) & (1 << 12), f'SPI{host} {name} callback/IRQ/timing failure')
            for direction in directions:
                descriptors = re.findall(rf'^SPI_NATIVE_DESCRIPTOR host={host} name={name} direction={direction} index=(\d+) address=([0-9a-f]+) before=([0-9a-f]+) after=([0-9a-f]+)$', text, re.MULTILINE)
                require([int(index) for index, *_ in descriptors] == [0, 1, 2], f'SPI{host} {name} requires actual three-descriptor chain')
                require(all(int(before, 16) & (1 << 31) for _, _, before, _ in descriptors), f'SPI{host} {name} descriptors not initially DMA owned')
                require(sum((int(after, 16) >> 12) & 4095 for _, _, _, after in descriptors) == LARGE_BYTES,
                        f'SPI{host} {name} descriptor received byte count differs')
                last_address = int(descriptors[-1][1], 16)
                last = int(descriptors[-1][3], 16)
                if direction == 'RX':
                    require(not last & (1 << 31) and last & (1 << 30) and (last >> 12) & 4095 == 17 and last & 4095 == 20,
                            f'SPI{host} {name} RX partial tail must have length17 capacity20 owner0 EOF1')
                    require(all(not int(after, 16) & ((1 << 31) | (1 << 30)) for _, _, _, after in descriptors[:-1]),
                            f'SPI{host} {name} earlier RX descriptors retained owner or premature EOF')
                    require(int(event['rx_eof'], 16) == last_address and int(event['rx_raw'], 16) & 2,
                            f'SPI{host} {name} actual RX EOF pointer/raw IRQ mismatch')
                else:
                    require(int(event['tx_eof'], 16) == last_address and int(event['tx_raw'], 16) & 8,
                            f'SPI{host} {name} actual TX EOF pointer/raw IRQ mismatch')
                    if int(event['tx_conf'], 16) & 4:
                        require(all(not int(after, 16) & (1 << 31) for _, _, _, after in descriptors),
                                f'SPI{host} {name} enabled TX writeback failed')
    if mode == 'loopback':
        names = ('direct_full_then_empty', 'direct_partial3', 'direct_zero_bytes', 'direct_reset_empty')
        for index, name in enumerate(names):
            matches = re.findall(rf'^SPI_NATIVE_DIRECT host=3 name={name} (.*)$', text, re.MULTILINE)
            require(len(matches) == 1, f'Missing actual SPI3 RX completion boundary {name}')
            event = dict(re.findall(r'(\w+)=([0-9a-f]+)', matches[0]))
            require(event['next_unchanged'] == '1' and event['d1_before'] == event['d1_after'] == '80000004',
                    f'{name} changed preloaded-but-empty next descriptor')
            after, raw = int(event['after'], 16), int(event['rx_raw'], 16)
            if index < 2:
                size = 4 if index == 0 else 3
                require(after == 4 | (size << 12) | (1 << 30) and raw & 2
                        and int(event['eof'], 16) == int(event['d0_address'], 16)
                        and int(event['spi_raw'], 16) & (1 << 12),
                        f'{name} failed actual partial/full RX EOF ownership/length/IRQ')
                expected = bytes.fromhex('619a35c7')[:size] + b'\xa5' * (8 - size)
            else:
                require(event['before'] == event['after'] == '80000004' and not raw & 2,
                        f'{name} falsely completed an empty RX descriptor')
                expected = b'\xa5' * 8
            require(rows.get(name) == expected, f'{name} actual physical loopback bytes incorrect')
        require(checks.get('direct_no_duplicate_eof') == 'PASS', 'Missing no-duplicate RX EOF check')


def validate_negative(text, mode, host, runtime_status, diagnostics, registers, graph):
    expected = 'strict_pause_owner_error' if mode == 'owner_error' else 'strict_pause_unknown_miso'
    require(re.search(rf'^SPI_NATIVE_NEGATIVE_ARM host={host} profile={mode} virtual_us=\d+ expected={expected}$', text, re.MULTILINE),
            'Strict pause occurred before the negative transfer was armed; not a qualified SPI negative')
    require('SPI_NATIVE_DONE ' not in text and 'unexpected_negative_rx' not in text, 'Negative transfer manufactured an RX completion')
    require(runtime_status.get('status') == 'paused' and not runtime_status.get('running'), 'VM did not strict-pause')
    reason = 'RX GDMA unavailable/short/owner or address error' if mode == 'owner_error' else 'MISO disconnected/floating/contended/unknown'
    require(re.search(rf'SPI{host} strict dependency: {re.escape(reason)} at \d+ ns', diagnostics),
            'Missing exact native SPI sample dependency diagnostic; GPIO/boot/dependency pauses do not qualify')
    require(registers['command'] & (1 << 24) and not registers['interrupt_raw'] & (1 << 12),
            'Unknown MISO completed the controller instead of preserving incomplete USR/DONE state')
    if mode == 'owner_error':
        require(registers['interrupt_raw'] & (1 << 17) and registers['rx_raw'] & 8 and not registers['rx_raw'] & 2,
                'Owner error lacks actual RX_FULL/DSCR_ERR or fabricated successful EOF')
        require(registers['d0_0'] == 4 and registers['d1_0'] == 0x80000004
                and registers['buffer_0'] == registers['buffer_1'] == 0xa5a5a5a5,
                'Owner error modified unowned/empty descriptors or incoming payload')
        return
    miso = PINS[host][2]
    samples = [pad for pad in graph.get('pads', []) if pad.get('gpio') == miso]
    require(len(samples) == 1, f'Snapshot lacks exact target GPIO{miso} evidence')
    require(not samples[0].get('digital_valid') and
            (samples[0].get('floating') or samples[0].get('diagnostic') == 'indeterminate'),
            f'Snapshot target GPIO{miso} is not electrically unknown: {samples[0]}')


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()

def firmware_pin(args):
    fixture = ROOT / 'tests/firmware/spi_native'
    files = [fixture / 'CMakeLists.txt', fixture / 'main/CMakeLists.txt',
             fixture / 'main/Kconfig.projbuild', fixture / 'main/spi_native.c']
    files += sorted(fixture.glob('sdkconfig*.defaults'))
    sources = {str(path.relative_to(ROOT)): sha256(path) for path in files}
    pin_path = args.build_pin or args.flash.parent / 'build-pin.json'
    if args.record_build_pin:
        idf = args.idf.resolve(strict=True)
        commit = subprocess.check_output(['git', '-C', str(idf), 'rev-parse', 'HEAD'], text=True).strip()
        require(commit == IDF_COMMIT, 'Build provenance must use the pinned IDF6.1 commit')
        description = json.loads((args.flash.parent / 'project_description.json').read_text())
        require(pathlib.Path(description['idf_path']).resolve() == idf,
                'CMake build IDF path differs from the declared pinned source')
        pin = dict(idf_commit=commit, flash_sha256=sha256(args.flash),
                   sdkconfig_sha256=sha256(args.sdkconfig), sources=sources)
        pin_path.write_text(json.dumps(pin, indent=2) + '\n')
    pin = json.loads(pin_path.read_text())
    require(pin['idf_commit'] == IDF_COMMIT and pin['flash_sha256'] == sha256(args.flash)
            and pin['sdkconfig_sha256'] == sha256(args.sdkconfig) and pin['sources'] == sources,
            'Firmware build pin does not match pinned IDF/source/image/configuration')
    return pin


def runtime_pin(args):
    require(args.qemu is not None and args.runtime_source is not None,
            'Runtime execution requires --qemu and --runtime-source')
    record_path = args.runtime_source.resolve(strict=True)
    record = json.loads(record_path.read_text())
    source = pathlib.Path(record['source']).resolve(strict=True)
    require(source in args.qemu.parents, 'QEMU must belong to the recorded source checkout')
    identity = {key: record[key] for key in
                ('base_commit', 'frozen_prefix', 'dependency_record_sha256', 'inputs', 'copies', 'patches')}
    fingerprint = hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    require(fingerprint == record['fingerprint'], 'Runtime source fingerprint differs from its inputs')
    applied = record['applied_files']
    for relative, expected in applied.items():
        path = pathlib.PurePosixPath(relative)
        require(not path.is_absolute() and '..' not in path.parts,
                f'Unsafe applied-source path: {relative}')
        actual = source / relative
        require(actual.is_file() and sha256(actual) == expected,
                f'Recorded runtime source changed: {relative}')
    mapping = json.loads((pathlib.Path(__file__).parent / 'source-map.json').read_text())
    overlay_targets = {}
    for overlay in mapping.get('post_copy_overlays', []):
        name = 'prototypes/spi/' + overlay['source']
        require(sha256(pathlib.Path(__file__).parent / overlay['source']) == overlay['sha256']
                and record['inputs'].get(name) == overlay['sha256'],
                f'Runtime SPI overlay differs from its qualified input: {name}')
        operations = [item for item in record['patches'] if item['source'] == name]
        require(len(operations) == 1 and operations[0].get('after_copies') is True
                and operations[0].get('patch_context') == 'strict'
                and set(operations[0]['targets']) == set(overlay['targets'])
                and set(overlay['applied_sha256']) == set(overlay['targets']),
                f'Runtime SPI overlay ordering/scope differs: {name}')
        for target, expected in overlay['applied_sha256'].items():
            require(target not in overlay_targets, f'Duplicate SPI overlay target: {target}')
            overlay_targets[target] = expected
    for item in mapping['copies'] + mapping['graph_copies']:
        canonical = pathlib.Path(__file__).parent / item['source']
        expected = hashlib.sha256(canonical.read_bytes().replace(b'\r\n', b'\n')).hexdigest()
        require(record['inputs'].get('prototypes/spi/' + item['source']) == expected,
                f'Runtime SPI copied input differs from canonical source: {item["source"]}')
        require(applied.get(item['destination']) == overlay_targets.get(item['destination'], expected),
                f'Runtime does not contain qualified SPI source: {item["destination"]}')
    return dict(source=str(source), fingerprint=fingerprint,
                source_record=str(record_path), source_record_sha256=sha256(record_path),
                binary_sha256=sha256(args.qemu), verified_applied_files=len(applied))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=pathlib.Path)
    parser.add_argument('--runtime-source', type=pathlib.Path,
                        help='Hash-bound prepare.py source record for the selected consolidated QEMU')
    parser.add_argument('--flash', type=pathlib.Path, required=True)
    parser.add_argument('--sdkconfig', type=pathlib.Path, required=True)
    parser.add_argument('--mode', choices=PROFILES, required=True)
    parser.add_argument('--build-pin', type=pathlib.Path)
    parser.add_argument('--record-build-pin', action='store_true',
                        help='Record provenance after a real IDF build/merge; do not execute QEMU')
    parser.add_argument('--idf', type=pathlib.Path, default=ROOT / 'build-idf-6.1')
    parser.add_argument('--evidence', type=pathlib.Path, default=ROOT / 'build-runtime-state/spi-native-2026-10-07')
    parser.add_argument('--watchdog-seconds', type=float, default=180,
                        help='Bounded host watchdog only, independent of guest virtual timestamps')
    parser.add_argument('--icount', action='store_true',
                        help='Run with single-thread TCG and -icount shift=0,align=off,sleep=off, matching the consolidated timebase profile')
    args = parser.parse_args()
    # Bounded host watchdog only (never a guest timeout). Cap raised from 600
    # after the graph binary's real connected-fixture host duration exceeded
    # 600 s (per-edge native graph solving at 1 MHz SCLK plus UART drain).
    if not 1 <= args.watchdog_seconds <= 3600:
        parser.error('--watchdog-seconds must be 1..3600')
    args.evidence.mkdir(parents=True, exist_ok=True)
    evidence = pathlib.Path(tempfile.mkdtemp(prefix=f'{args.mode}-{time.strftime("%Y%m%dT%H%M%S")}-', dir=args.evidence.resolve()))
    live = pathlib.Path(tempfile.mkdtemp(prefix=f'spi-native-live-{args.mode}-', dir='/tmp'))
    uart, errors, output, guest_errors = [live / name for name in ('uart.log', 'stderr.log', 'stdout.log', 'guest-errors.log')]
    qtest_log = live / 'qtest.log'
    result = dict(status='FAIL', mode=args.mode, evidence=str(evidence), host_watchdog_seconds=args.watchdog_seconds,
                  qualification=dict(advanced='UNSUPPORTED', slave='UNSUPPORTED', psram_dma='UNSUPPORTED', spi01='UNTOUCHED'),
                  qmp_transcript=[], snapshots=[], command=[])
    started = time.monotonic()
    deadline = started + args.watchdog_seconds
    proc = qmp = None
    try:
        if not args.record_build_pin:
            require(args.qemu is not None, 'Runtime execution requires --qemu')
        names = ('flash', 'sdkconfig') if args.record_build_pin else ('qemu', 'flash', 'sdkconfig')
        for name in names:
            path = getattr(args, name).resolve(strict=True)
            require(path.is_file(), f'--{name} must name a file')
            setattr(args, name, path)
        if not args.record_build_pin:
            result['runtime_pin'] = runtime_pin(args)
        config = configuration(args.sdkconfig, args.mode)
        result['firmware_configuration'] = config
        result['firmware_build_pin'] = firmware_pin(args)
        if args.record_build_pin:
            result['status'] = 'BUILD_PIN_RECORDED'
            return 0
        graph = project(args.mode, config['negative_host'])
        (evidence / 'project.json').write_text(json.dumps(graph, indent=2) + '\n')
        with tempfile.TemporaryDirectory(prefix='spi-native-transport-') as transport:
            qmp_path, qtest_path = (pathlib.Path(transport) / name for name in ('qmp.sock', 'qtest.sock'))
            command = [str(args.qemu), '-machine', 'esp32s3', '-nographic', '-S', '-monitor', 'none',
                       '-serial', f'file:{uart}', '-drive', f'file={args.flash},if=mtd,format=raw,snapshot=on',
                       '-qmp', f'unix:{qmp_path},server=on,wait=off', '-qtest', f'unix:{qtest_path},server=on,wait=off',
                       '-qtest-log', str(qtest_log), '-d', 'guest_errors', '-D', str(guest_errors)]
            if args.icount:
                command += ['-accel', 'tcg,thread=single', '-icount', 'shift=0,align=off,sleep=off']
            result['command'] = command
            (evidence / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            with output.open('wb') as stdout, errors.open('wb') as stderr:
                proc = subprocess.Popen(command, stdout=stdout, stderr=stderr)
                while not qmp_path.exists() or not qtest_path.exists():
                    if proc.poll() is not None:
                        raise RuntimeError(f'QEMU exited {proc.returncode} before native transports; see stderr.log')
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Host watchdog expired waiting for native transports')
                    time.sleep(.05)
                qmp = Qmp(qmp_path, result['qmp_transcript'], deadline)
                qmp.call('stop')
                try:
                    qmp.call('qom-set', dict(path='/machine/soc/electrical', property='project-json', value=json.dumps(graph)))
                except RuntimeError as exc:
                    result['status'] = 'PREREQUISITE'
                    raise RuntimeError(f'Exact prerequisite: source-bound binary must include native v3 electrical graph and approved spi-nor-1m registry/binding with mosi/miso/sclk/cs/vdd/gnd and parameters={{}}. Native Apply rejected the real graph: {exc}') from exc
                applied = qmp.snapshot()
                result['snapshots'].append(dict(phase='applied-before-boot', graph=applied))
                qmp.call('cont')
                negative = args.mode in ('wrong_cs', 'disconnected_miso', 'owner_error')
                while True:
                    text = uart.read_text(errors='replace') if uart.exists() else ''
                    runtime = qmp.call('query-status')
                    if re.search(r'^SPI_NATIVE_DONE [^\r\n]*\n', text, re.MULTILINE):
                        require(not negative, 'Strict-pause negative unexpectedly reached DONE')
                        qmp.call('stop')
                        final = qmp.snapshot()
                        result['snapshots'].append(dict(phase='firmware-completed', graph=final))
                        validate_uart(text, config)
                        result['status'] = 'PASS'
                        break
                    if runtime['status'] == 'paused':
                        final = qmp.snapshot()
                        result['snapshots'].append(dict(phase='strict-pause', graph=final))
                        diagnostics = '\n'.join(path.read_text(errors='replace') for path in (errors, guest_errors) if path.exists())
                        require(negative, f'Connected fixture strict-paused; exact prerequisite/diagnostic: {diagnostics[-2000:]}')
                        registers = read_registers(qtest_path, config['negative_host'], deadline, text, args.mode == 'owner_error')
                        result['paused_registers'] = registers
                        validate_negative(text, args.mode, config['negative_host'], runtime, diagnostics, registers, final)
                        result['status'] = 'NEGATIVE_STRICT_PAUSE'
                        result['negative_outcome'] = ('Expected owner-error strict pause; not driver transaction success'
                                                      if args.mode == 'owner_error' else
                                                      'Expected missing-MISO strict pause; not driver transaction success')
                        break
                    if proc.poll() is not None:
                        raise RuntimeError(f'QEMU exited {proc.returncode} before fixture outcome')
                    if time.monotonic() >= deadline:
                        raise TimeoutError('Host watchdog expired; guest result unqualified, never treated as a passing SPI timeout')
                    time.sleep(.05)
    except Exception as exc:
        result['error'] = f'{type(exc).__name__}: {exc}'
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
            result['qemu_returncode_after_cleanup'] = proc.returncode
        # Producer has exited; archive native /tmp live producer logs durably.
        for live_file in (uart, errors, output, guest_errors, qtest_log):
            if live_file.exists():
                shutil.copy2(live_file, evidence / live_file.name)
        shutil.rmtree(live, ignore_errors=True)
        result['live_transport'] = 'native /tmp during producer life; archived to evidence after exit'
        result['host_elapsed_seconds'] = time.monotonic() - started
        archived_uart = evidence / 'uart.log'
        result['uart'] = observations(archived_uart.read_text(errors='replace') if archived_uart.exists() else '')
        sources = [pathlib.Path(__file__).resolve()]
        sources.extend(path for path in (args.qemu, args.flash, args.sdkconfig)
                       if path is not None and path.is_file())
        sources.extend(path for path in evidence.iterdir() if path.is_file())
        result['hashes'] = {str(path): sha256(path) for path in sources}
        (evidence / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(dict(status=result['status'], evidence=str(evidence), error=result.get('error'))))
    return 0 if result['status'] in ('PASS', 'NEGATIVE_STRICT_PAUSE') else 2 if result['status'] == 'PREREQUISITE' else 1


if __name__ == '__main__':
    raise SystemExit(main())
