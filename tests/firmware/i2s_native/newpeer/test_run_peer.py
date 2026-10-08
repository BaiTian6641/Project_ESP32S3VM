"""Synthetic launcher contract/parser/math tests; no QEMU or playback proof."""
import json
from pathlib import Path
import unittest
import tempfile
from types import SimpleNamespace
from unittest import mock

if __package__:
    from . import graphs, run_peer
else:
    import graphs
    import run_peer


class SyntheticLauncherContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = json.loads((Path(__file__).parent / 'cases-v1.json').read_bytes())['cases']

    def test_all_68_ready_geometries_and_stable_ids(self):
        for i, case in enumerate(self.cases):
            ready = run_peer.expected_geometry(case, i)
            self.assertEqual(int(ready['id']), i)
            self.assertEqual(ready['name'], case['name'])
            c = case['cmake']
            self.assertEqual(int(ready['bytes']), 96 * c['PEER_MASK'].bit_count() * ((c['PEER_BITS'] + 7) // 8))

    def test_canonical_peer_kind_and_all_136_manifest_graphs(self):
        for case in self.cases:
            peer_document = graphs.project(case['name'], 'peer')
            peer = next(c for c in peer_document['components'] if c['id'] == 'Peer')
            self.assertEqual(peer['type'], 'i2s-sample-peer')
            self.assertEqual(peer['kind'], 'device')
            for stage, key in (('peer', 'peerGraph'), ('boot', 'bootGraph')):
                saved = json.loads((Path(__file__).parent / case[key]).read_bytes())
                self.assertEqual(saved, graphs.project(case['name'], stage))

    def test_negative_profiles_preserve_mcu_power_and_real_wires(self):
        for name in (self.cases[0]['name'], next(c['name'] for c in self.cases if c['name'].startswith('raw-pdm'))):
            original = graphs.project(name)
            for profile in run_peer.PROFILES:
                actual = run_peer.graph_profile(original, profile)
                vdd = next(n for n in actual['nets'] if n['id'] == 'vdd')['endpoints']
                self.assertIn('U1.vdd', vdd)
                self.assertIn('V.p', vdd)
                self.assertEqual(next(n for n in original['nets'] if n['id'] == 'vdd')['endpoints'], ['V.p', 'U1.vdd', 'Peer.vdd'])
                peer = next(c for c in actual['components'] if c['id'] == 'Peer')
                config = peer['attributes']['native_i2s_peer']
                if profile == 'exhaustion':
                    self.assertFalse(config['repeat'])
                elif profile == 'capture-overflow':
                    self.assertEqual(config['captureCapacity'], 32)
                elif profile == 'power-off':
                    self.assertNotIn('Peer.vdd', vdd)
                    self.assertIn('Peer.vdd', next(n for n in actual['nets'] if n['id'] == 'gnd')['endpoints'])
                elif profile == 'power-unknown':
                    self.assertEqual(next(n for n in actual['nets'] if n['id'] == 'unknown-peer-vdd')['endpoints'], ['Peer.vdd'])
                else:
                    self.assertIn('Peer.vdd', vdd)

    def test_boundary_status_never_accepts_arbitrary_failures(self):
        for profile in run_peer.PROFILES:
            self.assertFalse(run_peer.negative_boundary(profile, dict(activation_error='missing model')))
        self.assertTrue(run_peer.negative_boundary('power-unknown', dict(power_known=False, powered=False)))
        self.assertFalse(run_peer.negative_boundary('power-unknown', dict(power_known=False, powered=True)))
        self.assertTrue(run_peer.negative_boundary('power-off', dict(power_known=True, registered_powered=False, powered=False)))
        self.assertFalse(run_peer.negative_boundary('power-off', dict(power_known=False, registered_powered=False, powered=False)))
        self.assertTrue(run_peer.negative_boundary('exhaustion', dict(exhausted=True)))
        self.assertTrue(run_peer.negative_boundary('capture-overflow', dict(capture_overflow=True)))

    def test_exact_uart_tags_not_substring_success(self):
        text = 'noise I2S_PEER_DONE failures=0\nI2S_PEER_DONE failures=3\nI2S_PEER_CASE_ERROR invalid-command\n'
        self.assertEqual(run_peer.records(text, 'I2S_PEER_DONE'), [dict(failures='3')])
        self.assertEqual(run_peer.records(text, 'I2S_PEER_CASE'), [])

    def test_synthetic_transition_metadata_reports_loss_not_fabricated_origin(self):
        value = dict(version=1, capacity=8, count=8, first_index=3,
                     first_sequence=19, total=27, lost=19, epoch=4, capture_enabled=True)
        self.assertEqual(run_peer.transition_metadata(value)['lost'], 19)
        self.assertNotIn('capture_enabled', run_peer.transition_metadata(value))
        for change in (dict(first_sequence=18), dict(count=7), dict(first_index=8),
                       dict(capacity=0), dict(total=26), dict(lost=True)):
            with self.assertRaises(ValueError):
                run_peer.transition_metadata(dict(value, **change))

    def test_synthetic_closed_transition_pages_start_at_discovered_retained_sequence(self):
        class SyntheticTransport:
            def __init__(self):
                self.request = None
                self.requests = []
                common = dict(version=1, capacity=2048, first_index=0, epoch=3, activation_ns=123)
                self.peer = dict(common, count=1025, first_sequence=7, total=1032, lost=7,
                                 generation=2, power_on_ns=100, config_activation_ns=101)
                self.core = dict(common, controller=0, active=False, count=3, first_sequence=2, total=5, lost=2)

            def call(self, command):
                return dict(running=False)

            def set(self, path, prop, request):
                self.request = request
                self.requests.append(request)

            def get(self, path, prop):
                if prop == 'status-json':
                    return dict(version=1, kind='i2s-peer-status',
                                peers=[dict(component_id='Peer', config_identity='a' * 64, transitions=self.peer)],
                                controllers=[dict(self.core, capture_enabled=True)])
                request = self.request['transitions']
                scopes = {}
                for scope, meta, field in (('peer', self.peer, 'peerOffset'), ('controller', self.core, 'controllerOffset')):
                    offset = request[field]
                    records = [dict(sequence=i) for i in range(offset, min(meta['total'], offset + request['count']))]
                    scopes[scope] = dict(meta, offset=offset, records=records)
                return dict(version=1, kind='i2s-peer-din-capture', component_id='Peer',
                            config_identity='a' * 64, total=1234, offset=1234, events=[],
                            transitions=dict(version=1, **scopes))

        transport = SyntheticTransport()
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'synthetic-pages.json'
            result = run_peer.export_transitions(transport, path, 0, dict(total=1234, config_identity='a' * 64))
            pages = json.loads(path.read_bytes())
        self.assertEqual(len(pages), 2)
        self.assertEqual(transport.requests[0]['transitions']['peerOffset'], 7)
        self.assertEqual(transport.requests[0]['transitions']['controllerOffset'], 2)
        self.assertEqual(transport.requests[1]['transitions']['peerOffset'], 1031)
        self.assertEqual(transport.requests[1]['transitions']['controllerOffset'], 5)
        self.assertTrue(all(request['offset'] == 1234 and request['count'] == 1 for request in transport.requests))
        self.assertEqual(result['scopes']['peer']['lost'], 7)
        self.assertTrue(result['all_retained_records_exported'])

    def test_synthetic_epoch_selector_uses_first_fifo_load_not_attempts_or_values(self):
        first = dict(kind=6, source_word_id=1, epoch=2, word_ordinal=97)
        complete = dict(kind=7, source_word_id=1, epoch=3, word_ordinal=98)
        peer = dict(kind=3, epoch=4, source_cursor=0)
        pages = [dict(transitions=dict(controller=dict(records=[first, complete]),
                                       peer=dict(records=[peer])))]
        result = run_peer.transition_attribution(pages, 'a' * 64)
        self.assertEqual(result, dict(component_id='Peer', config_identity='a' * 64,
                                     controller_origin_epoch=2, controller_epochs=[3],
                                     peer_epochs=[4], min_frames=768))
        for records in ([complete], [dict(first, source_word_id=2), complete], [first, first, complete]):
            with self.assertRaises(ValueError):
                run_peer.transition_attribution(
                    [dict(transitions=dict(controller=dict(records=records), peer=dict(records=[peer])))],
                    'a' * 64)

    def test_synthetic_boot_failure_exports_electrical_snapshot_without_invented_peer(self):
        class SyntheticProcess:
            returncode = None

            def __init__(self, command, **kwargs):
                for option in ('-qmp', '-serial'):
                    uri = command[command.index(option) + 1]
                    Path(uri.split(':', 1)[1].split(',')[0]).touch()

            def poll(self):
                return self.returncode

            def terminate(self):
                self.returncode = 0

            def wait(self, timeout=None):
                return self.returncode

        class SyntheticQmp:
            def __init__(self, *args):
                pass

            def set(self, *args):
                pass

            def get(self, path, prop):
                if prop != 'snapshot-json':
                    raise AssertionError('synthetic boot has no applied peer')
                return dict(synthetic=True, boot_only=True)

            def call(self, *args):
                return {}

            def stop(self):
                pass

            def close(self):
                pass

        class SyntheticUart:
            def __init__(self, *args):
                pass

            def until(self, *args):
                raise ValueError('synthetic SELECT unavailable')

            def close(self):
                pass

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            flash = directory / 'synthetic-image.bin'
            flash.write_bytes(b'synthetic fixture, not ordinary IDF or runtime evidence')
            args = SimpleNamespace(flash=flash, qemu=directory / 'synthetic-qemu',
                                   profile='connected', timeout=1)
            with mock.patch.object(run_peer.subprocess, 'Popen', SyntheticProcess), \
                    mock.patch.object(run_peer, 'Qmp', SyntheticQmp), \
                    mock.patch.object(run_peer, 'Uart', SyntheticUart), \
                    mock.patch.object(run_peer, 'peer_status', side_effect=AssertionError('no boot peer')):
                result = run_peer.run_case(args, 0, self.cases[0], directory / 'case')
            snapshot = json.loads((directory / 'case' / 'failure-snapshot.json').read_bytes())
        self.assertEqual(result['status'], 'FAIL')
        self.assertFalse(result['physical_bidirectional_qualified'])
        self.assertIn('synthetic SELECT unavailable', result['error'])
        self.assertNotIn('failure_export_error', result)
        self.assertEqual(snapshot, dict(synthetic=True, boot_only=True))


if __name__ == '__main__':
    unittest.main()
