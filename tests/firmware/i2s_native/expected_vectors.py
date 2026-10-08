"""Host reference: mathematical samples, packed bytes, and rational cadence.

This module has no dependency on firmware output, controller implementation, or
IDF headers. An observed phase can select only a cyclic FRAME rotation, never
alter expected samples. No converter coefficients are guessed.
"""
from fractions import Fraction

FRAMES = 96
DESCRIPTORS = 4
WIDTHS = (8, 16, 24, 32)
FORMATS = ("philips", "msb", "pcm")


def matrix():
    rows = []
    for master in range(2):
        for fmt in FORMATS:
            for bits in WIDTHS:
                for mono in (0, 1):
                    rows.append(dict(mode="std", format=fmt, bits=bits,
                                     mask=1 if mono else 3, mono=mono,
                                     master=master, rate=12500))
    for master in range(2):
        for fmt in FORMATS:
            for bits in WIDTHS:
                for mask in ((0x81, 0x55, 0xff) if bits <= 16 else (0x09, 0x05, 0x0f)):
                    rows.append(dict(mode="tdm", format=fmt, bits=bits,
                                     mask=mask, mono=0, master=master, rate=12500))
    for source in range(2):
        for master in range(2):
            rows.append(dict(mode="pdm", format="raw", bits=16, mask=3,
                             mono=0, master=master, rate=1000000, source=source))
    return [dict(case=index, **row) for index, row in enumerate(rows)]


def vector(controller, bits, mask):
    samples = []
    limit = 2 ** bits
    active = [slot for slot in range(8) if (mask >> slot) & 1]
    for frame in range(FRAMES):
        for slot in active:
            affine = 73 * frame + 29 * slot + 101 * controller + 17
            mixed = 2654435769 * (1 + frame + slot)
            value = (affine ^ mixed) % limit
            samples.append(value.to_bytes(bits // 8, byteorder="little"))
    return b"".join(samples)


def fnv1a(payload):
    value = 2166136261
    for byte in payload:
        value = ((value ^ byte) * 16777619) % (2 ** 32)
    return f"{value:08x}"


def expected_capture(row, receiver, phase, rounds):
    if not 0 <= phase < FRAMES:
        raise ValueError(f"invalid frame phase {phase}")
    source = row.get("source", 1 - receiver)
    payload = vector(source, row["bits"], row["mask"])
    stride = row["mask"].bit_count() * row["bits"] // 8
    start = phase * stride
    rotated = payload[start:] + payload[:start]
    return dict(hash=fnv1a(rotated * rounds), bytes=len(payload) * rounds,
                frames=FRAMES * rounds)


def cadence(row):
    # Raw PDM codec line: two clock edges/bit pair, sixteen pairs per
    # stereo DMA frame. IDF config rate is NOT the DMA-frame sample rate.
    frame_hz = Fraction(row["rate"], 16) if row["mode"] == "pdm" else Fraction(row["rate"])
    frame_ns = Fraction(10 ** 9, 1) / frame_hz
    physical_slots = (8 if row["bits"] <= 16 else 4) if row["mode"] == "tdm" else 2
    bclk = Fraction(2 * row["rate"]) if row["mode"] == "pdm" else frame_hz * row["bits"] * physical_slots
    return dict(frame_hz=f"{frame_hz.numerator}/{frame_hz.denominator}",
                interval_ns=f"{frame_ns.numerator}/{frame_ns.denominator}",
                frame_us=Fraction(10 ** 6, 1) / frame_hz,
                bclk_hz=f"{bclk.numerator}/{bclk.denominator}")
