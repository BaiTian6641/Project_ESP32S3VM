"""SYNTHETIC parser/mathematics tests only: never runtime playback evidence."""
import json
from pathlib import Path
import unittest
if __package__:
    from . import peer_reference as ref
else:
    import peer_reference as ref


class SyntheticParserMath(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cases = json.loads((Path(__file__).parent / 'cases-v1.json').read_bytes())['cases']

    def case(self, prefix):
        return next(c for c in self.cases if c['name'].startswith(prefix))

    def test_manifest_68_roles_controllers(self):
        self.assertEqual(len(self.cases), 68)
        self.assertEqual({(c['cmake']['PEER_CONTROLLER'], c['cmake']['PEER_MASTER']) for c in self.cases}, {(0, 0), (0, 1), (1, 0), (1, 1)})

    def test_polynomial_fixed_words(self):
        self.assertEqual(ref.sample(0, 0, 0, 32), 0x9e3779a8)
        self.assertEqual(ref.sample(1, 0, 0, 32), 0x9e3779cf)
        self.assertEqual(ref.sample(0, 0, 0, 8), 0xa8)
        self.assertEqual(ref.word_bits(0xa8, 8), '10101000')

    def test_all_format_serial_lengths_and_source_independence(self):
        for c in self.cases:
            fmt, width, slots, mask = ref.geometry(c)
            self.assertEqual(len(ref.wire_frame(c, 0, 0)), slots * width)
            self.assertNotEqual(ref.wire_frame(c, 0, 0), ref.wire_frame(c, 1, 0))
            words = ref.expected_events(c, 1, 95, 2)
            chosen = ref.selected_slots(slots, mask)
            self.assertEqual([slot for _, slot in words], chosen * 2)
            self.assertEqual(words[len(chosen)][0], ref.sample(1, 0, chosen[0], width))

    def test_raw_first_phase_zero_chronology_16_words(self):
        c = self.case('raw-pdm-16')
        events = ref.expected_events(c, 0, 0, 16, True)
        self.assertEqual(len(events), 16 * 32)
        self.assertEqual([s for _, s in events], [0, 1] * (16 * 16))
        for frame in range(16):
            for slot in (0, 1):
                text = ''.join(str(events[frame * 32 + bit * 2 + slot][0]) for bit in range(16))
                self.assertEqual(int(text, 2), ref.sample(0, frame, slot, 16))
        self.assertEqual(ref.offsets(c, 1, True)[1], 500)
        self.assertEqual(ref.offsets(c, 1, False), [0, 0])
        peer_master = self.case('raw-pdm-16-mcu-slave')
        self.assertEqual(ref.offsets(peer_master, 1, True)[:3], [250, 750, 1250])
        self.assertEqual(ref.offsets(c, 1, True)[:3], [0, 500, 1000])

    def test_tdm_sparse_physical_slot_cadence(self):
        c = self.case('tdm-8-s16')
        self.assertEqual(ref.selected_slots(16, 0x8181), [0, 7, 8, 15])
        self.assertEqual(ref.offsets(c, 2, False), [0, 35000, 40000, 75000, 80000, 115000, 120000, 155000])

    def test_synthetic_format_ws_alignment(self):
        philips = self.case('philips-8')
        msb = self.case('msb-8')
        pcm = self.case('pcm-8')
        self.assertEqual(ref.serial_timeline(philips, 0, 1)[1], (0,) * 7 + (1,) * 8 + (0,))
        self.assertEqual(ref.serial_timeline(msb, 0, 1)[1], (0,) * 8 + (1,) * 8)
        self.assertEqual(ref.serial_timeline(pcm, 0, 1)[1], (0,) * 15 + (1,))

    def test_rx_header_geometry_and_truncation(self):
        header = ref.HEADER.pack(b'S3I2SRX1', 1, 1)
        event = ref.EVENT.pack(12, 0, 0, 0xff, 15, 8, 3)
        controller, events = ref.parse_rx(header + event)
        self.assertEqual(controller, 1)
        self.assertEqual(events[0]['slot'], 15)
        for data in (b'', header, header + event[:-1], ref.HEADER.pack(b'badmagic', 0, 1) + event):
            with self.assertRaises(ValueError):
                ref.parse_rx(data)

    def test_rx_rejects_missing_sequence_and_unknown_flags(self):
        header = ref.HEADER.pack(b'S3I2SRX1', 0, 1)
        first = ref.EVENT.pack(0, 0, 0, 0, 0, 16, 3)
        for second in (ref.EVENT.pack(1, 2, 0, 0, 1, 16, 3), ref.EVENT.pack(1, 1, 0, 0, 1, 16, 1)):
            with self.assertRaises(ValueError):
                ref.parse_rx(header + first + second)

    def test_synthetic_actual_window_schema_parser_only(self):
        export = dict(version=1, kind='i2s-peer-din-capture',
                      component_id='P1', config_identity='a' * 64, offset=0, total=1, status={},
                      events=[dict(ns=500, sequence=0, sample=1, slot=0, raw=True)])
        _, events = ref.parse_din(json.dumps(export).encode())
        self.assertEqual(events[0]['sample'], 1)
        export['events'][0]['sample'] = 2
        with self.assertRaises(ValueError):
            ref.parse_din(json.dumps(export).encode())

    def test_synthetic_capture_window_gaps_and_identity(self):
        first = dict(version=1, kind='i2s-peer-din-capture', component_id='P1',
                     config_identity='a' * 64, offset=0, total=2, status={},
                     events=[dict(ns=500, sequence=0, sample=1, slot=0, raw=True)])
        second = dict(first, offset=1, events=[dict(ns=1000, sequence=1, sample=0, slot=1, raw=True)])
        self.assertEqual(len(ref.parse_din(json.dumps([first, second]).encode())[1]), 2)
        for pages in ([first], [second, first], [first, first],
                      [first, dict(second, config_identity='b' * 64)],
                      [first, dict(second, total=3)]):
            with self.assertRaises(ValueError):
                ref.parse_din(json.dumps(pages).encode())

    def test_synthetic_philips_complete_frame_not_initial_tail(self):
        c = self.case('philips-8')
        expected = ref.expected_events(c, 1, 0, 768)
        times = ref.offsets(c, 768, False)
        events = [dict(ns=int(t), sequence=i, frame=i // 2 + int(s == 1),
                       sample=v, slot=s, width=8, flags=3)
                  for i, ((v, s), t) in enumerate(zip(expected, times))]
        spec = dict(start=0, stop=len(events), phase=0, frames=768)
        self.assertTrue(ref.qualify_direction(c, spec, events, 1, False)['exact_slot_order'])
        events[1]['frame'] -= 1
        with self.assertRaises(ValueError):
            ref.qualify_direction(c, spec, events, 1, False)

    def test_synthetic_finite_direction_and_cadence_rejection(self):
        c = self.case('msb-8')
        expected = ref.expected_events(c, 1, 0, 768)
        times = ref.offsets(c, 768, False)
        events = [dict(ns=int(t), sequence=i, frame=i // 2, sample=v, slot=s, width=8, flags=3)
                  for i, ((v, s), t) in enumerate(zip(expected, times))]
        spec = dict(start=0, stop=len(events), phase=0, frames=768)
        self.assertTrue(ref.qualify_direction(c, spec, events, 1, False)['exact_samples'])
        events[9]['ns'] += 2
        with self.assertRaises(ValueError):
            ref.qualify_direction(c, spec, events, 1, False)
        events[9]['ns'] -= 2
        events[9]['sample'] ^= 1
        with self.assertRaises(ValueError):
            ref.qualify_direction(c, spec, events, 1, False)

    def transition_fixture(self):
        """Tiny synthetic metadata only; no runtime/native success claim."""
        peer = [dict(sequence=0, ns=0, epoch=4, kind=0)]
        controller = [dict(sequence=0, ns=0, epoch=7, kind=0)]
        for slot in (0, 1):
            time = 100 + slot * 40000
            shared = dict(epoch=4, source_id=slot + 1, source_cursor=slot,
                          raw_channel=0xffffffff, slot=slot, source=0, raw=False,
                          powered=True, power_known=True, valid_bits=8, slot_bits=8,
                          first_ns=time - 50, last_ns=time - 50, last_boundary_ns=time,
                          published_bits=8, frame=0)
            peer.append(dict(shared, sequence=len(peer), ns=time - 50, kind=2))
            peer.append(dict(shared, sequence=len(peer), ns=time, kind=3))
            identity = 100 + slot
            controller.append(dict(sequence=len(controller), ns=time - 50, epoch=7,
                                   kind=6, source_word_id=identity))
            controller.append(dict(sequence=len(controller), ns=time, epoch=7,
                                   kind=7, source_word_id=identity, word_ordinal=900 + slot,
                                   flags=0, source=0, slot=slot, raw_channel=0xffffffff,
                                   valid_bits=8, shifted_bits=8, slot_bits=8, first_ns=time - 50,
                                   last_ns=time - 50, first_boundary_ns=time - 35000,
                                   last_boundary_ns=time, frame=0))
        transitions = {scope: dict(records=records, metadata={})
                       for scope, records in (('peer', peer), ('controller', controller))}
        attribution = dict(controller_origin_epoch=7, controller_epochs=[7], peer_epochs=[4])
        return transitions, attribution

    def test_synthetic_ids_not_attempt_ordinals_and_startup_retention(self):
        case = self.case('msb-8')
        transitions, attribution = self.transition_fixture()
        mapping = ref._source_expectations(case, transitions, attribution)
        self.assertEqual(mapping['origin_id'], 100)
        self.assertEqual(mapping['din'][(100, 0)]['sample'], ref.sample(0, 0, 0, 8))
        self.assertEqual(mapping['din'][(40100, 1)]['sample'], ref.sample(0, 0, 1, 8))
        events = [dict(ns=0, sequence=0, sample=0, slot=0, raw=False)]
        events += [dict(ns=time, sequence=slot + 1, sample=ref.sample(0, 0, slot, 8),
                        slot=slot, raw=False) for slot, time in ((0, 100), (1, 40100))]
        evidence = ref._qualify_mapped(case, events, mapping['din'], 'din', mapping, 1)
        self.assertEqual(evidence['actual_events'], 3)
        self.assertEqual(evidence['qualified_count'], 2)
        self.assertEqual(evidence['classifications'][0]['stop'], 1)
        events[1]['sample'] ^= 1
        with self.assertRaises(ValueError):
            ref._qualify_mapped(case, events, mapping['din'], 'din', mapping, 1)

    def test_synthetic_lost_load_and_begin_fail_attribution(self):
        case = self.case('msb-8')
        for scope, kind in (('controller', 6), ('peer', 2)):
            transitions, attribution = self.transition_fixture()
            transitions[scope]['records'] = [r for r in transitions[scope]['records']
                                              if not (r['kind'] == kind and r['sequence'] == 1)]
            with self.assertRaises(ValueError):
                ref._source_expectations(case, transitions, attribution)

    def test_synthetic_raw_transition_chronology_both_roles(self):
        for role in ('master', 'slave'):
            case = self.case('raw-pdm-16-mcu-' + role)
            peer = [dict(sequence=0, ns=0, epoch=4, kind=0)]
            for cursor in range(32):
                time = 1000 + cursor * 500
                record = dict(epoch=4, source_id=cursor + 1, source_cursor=cursor,
                              raw_channel=cursor % 2, slot=cursor // 16, source=0, raw=True,
                              powered=True, power_known=True, valid_bits=1, slot_bits=1,
                              first_ns=time, last_ns=time, last_boundary_ns=time,
                              published_bits=1, frame=0)
                peer.append(dict(record, sequence=len(peer), ns=time, kind=2))
                peer.append(dict(record, sequence=len(peer), ns=time, kind=3))
            controller = [dict(sequence=0, ns=0, epoch=7, kind=0)]
            for slot in (0, 1):
                first = 1000 + slot * 500
                last = first + 15000
                controller.append(dict(sequence=len(controller), ns=first, epoch=7,
                                       kind=6, source_word_id=1000 + slot))
                controller.append(dict(sequence=len(controller), ns=last + 250, epoch=7,
                                       kind=7, source_word_id=1000 + slot, word_ordinal=300 + slot,
                                       flags=1 | (2 if role == 'slave' else 0), source=0,
                                       slot=slot, raw_channel=slot, valid_bits=16,
                                       shifted_bits=16, slot_bits=16, first_ns=first,
                                       last_ns=last, first_boundary_ns=-1 if role == 'slave' else first + 250,
                                       last_boundary_ns=-1 if role == 'slave' else last + 250))
            controller.sort(key=lambda r: r['ns'])
            for sequence, record in enumerate(controller):
                record['sequence'] = sequence
            transitions = dict(peer=dict(records=peer), controller=dict(records=controller))
            attribution = dict(controller_origin_epoch=7, controller_epochs=[7], peer_epochs=[4])
            mapping = ref._source_expectations(case, transitions, attribution)
            for slot in (0, 1):
                self.assertEqual(mapping['rx'][(16750, slot)]['sample'], ref.sample(1, 0, slot, 16))
            delay = 250 if role == 'slave' else 0
            events = []
            for bit in range(16):
                for slot in (0, 1):
                    value = int(ref.word_bits(ref.sample(0, 0, slot, 16), 16)[bit])
                    events.append(dict(ns=1000 + bit * 1000 + slot * 500 + delay,
                                       sequence=len(events), sample=value, slot=slot, raw=True))
            evidence = ref._qualify_mapped(case, events, mapping['din'], 'din', mapping, 1)
            self.assertEqual(evidence['qualified_count'], 32)

    def test_synthetic_transition_page_loss_and_gap_accounting(self):
        window = dict(version=1, first_sequence=2, total=4, lost=2, capacity=16,
                      count=2, offset=2, records=[dict(sequence=2, ns=10)])
        next_window = dict(window, offset=3, records=[dict(sequence=3, ns=20)])
        pages = [dict(version=1, peer=window, controller=window),
                 dict(version=1, peer=next_window, controller=next_window)]
        parsed = ref.parse_transition_pages(json.dumps(pages).encode())
        self.assertEqual(parsed['peer']['metadata']['lost'], 2)
        self.assertEqual(len(parsed['controller']['records']), 2)
        for invalid in (pages[:1], [pages[1], pages[0]], [pages[0], pages[0]]):
            with self.assertRaises(ValueError):
                ref.parse_transition_pages(json.dumps(invalid).encode())


if __name__ == '__main__':
    unittest.main()
