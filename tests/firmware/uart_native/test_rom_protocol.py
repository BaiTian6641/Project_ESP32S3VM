"""Host-only protocol boundaries; synthetic bytes are not native ROM evidence."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "uart_rom_protocol", ROOT / "qemu-extensions/prototypes/uart/run-rom-download.py")
protocol = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(protocol)


def packet(direction, opcode, data, value=0):
    raw = struct.pack("<BBHI", direction, opcode, len(data), value) + data
    return b"\xc0" + raw.replace(b"\xdb", b"\xdb\xdd").replace(b"\xc0", b"\xdb\xdc") + b"\xc0"


class ProtocolProofTests(unittest.TestCase):
    def proof(self, attempts, replies, memory_ops=(5, 7, 7, 6), error_reply=None):
        payload = bytes(range(256)) + bytes(range(44))
        address, block_size = 0x40378000, 256
        sync = packet(0, 8, b"\x07\x07\x12\x20" + b"\x55" * 32)
        sent = sync * attempts + packet(0, 5, struct.pack("<IIII", len(payload), 2, block_size, address))
        for sequence in range(2):
            data = payload[sequence * block_size:(sequence + 1) * block_size]
            checksum = 0xef
            for byte in data:
                checksum ^= byte
            sent += packet(0, 7, struct.pack("<IIII", len(data), sequence, 0, 0) + data, checksum)
        sent += packet(0, 6, struct.pack("<II", 1, 0))
        incoming = b"ESP-ROM:esp32s3-20210327\r\nwaiting for download\r\n"
        for index, opcode in enumerate([8] * replies + list(memory_ops)):
            status = b"\x01\x07\x00\x00" if index == error_reply else b"\x00" * 4
            incoming += packet(1, opcode, status, 0x20121207)
        commands = [{"opcode": 8, "error": "acquisition timeout"}] * (attempts - 1)
        commands += [{"opcode": 8, "reply": {}}] + [{"opcode": None, "reply": {}}] * 7
        commands += [{"opcode": opcode, "reply": {}} for opcode in (5, 7, 7, 6)]
        with tempfile.TemporaryDirectory() as directory:
            evidence = Path(directory)
            (evidence / "host-to-rom.bin").write_bytes(sent)
            (evidence / "rom-to-host.bin").write_bytes(incoming)
            return protocol.protocol_proof(evidence, payload, address, block_size, commands)

    def test_complete_groups_from_legal_acquisition_retries(self):
        for attempts, replies in ((1, 8), (4, 16), (5, 40)):
            with self.subTest(attempts=attempts, replies=replies):
                result = self.proof(attempts, replies)
                self.assertEqual(result["sync_requests"], attempts)
                self.assertEqual(result["sync_reply_groups"], replies // 8)
                self.assertEqual([row["opcode"] for row in result["responses"]],
                                 [8] * replies + [5, 7, 7, 6])

    def test_missing_partial_and_unsolicited_groups_are_rejected(self):
        for attempts, replies in ((1, 0), (4, 7), (4, 9), (1, 16), (4, 40)):
            with self.subTest(attempts=attempts, replies=replies):
                with self.assertRaisesRegex(RuntimeError, "SYNC reply group"):
                    self.proof(attempts, replies)

    def test_extra_sync_does_not_hide_missing_or_reordered_memory_ack(self):
        for memory_ops in ((5, 7, 6), (5, 8, 7, 7, 6), (5, 7, 6, 7)):
            with self.subTest(memory_ops=memory_ops):
                with self.assertRaisesRegex(RuntimeError, "memory ACK traversal"):
                    self.proof(4, 16, memory_ops=memory_ops)

    def test_every_reply_group_status_is_checked(self):
        with self.assertRaisesRegex(RuntimeError, "failure status"):
            self.proof(4, 16, error_reply=8)


if __name__ == "__main__":
    unittest.main()
