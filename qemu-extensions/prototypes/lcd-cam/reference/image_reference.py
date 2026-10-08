#!/usr/bin/env python3
"""Independent native LCD/CAM image oracles; never import a device encoder.

Raw formats compare exact bytes. JPEG compares an independently decoded RGB
image, NOT compressor bytes. All dimensions and source selections are expected
inputs supplied by the capture owner, never learned from the image under test.
"""
import io
import struct

MAX_PIXELS = 1600 * 1200
MAX_PAYLOAD = 16 * 1024 * 1024
BARS = ((255, 255, 255), (255, 255, 0), (0, 255, 255), (0, 255, 0),
        (255, 0, 255), (255, 0, 0), (0, 0, 255), (0, 0, 0))


class ReferenceError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise ReferenceError(message)


def dimensions(width, height):
    require(type(width) is int and type(height) is int, 'dimensions must be integers')
    require(0 < width <= 1600 and 0 < height <= 1200 and width * height <= MAX_PIXELS,
            'dimensions exceed the bounded image contract')


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def expand565(word):
    r, g, b = word >> 11, (word >> 5) & 63, word & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def rgb332(rgb):
    r, g, b = rgb
    return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)


def expand332(value):
    r, g, b = value >> 5, (value >> 2) & 7, value & 3
    return ((r << 5) | (r << 2) | (r >> 1),
            (g << 5) | (g << 2) | (g >> 1), b * 85)


def pattern_pixel(pattern, x, y, width, height):
    if isinstance(pattern, str):
        pattern = {'source': pattern}
    source = pattern['source']
    if source in ('lcd-rgb332-fixture', 'lcd-rgb888-fixture'):
        frame = pattern['frame']
        require(type(frame) is int and 0 <= frame <= 2, 'invalid RGB fixture frame')
        if source == 'lcd-rgb332-fixture':
            return expand332((((x + frame * 3) & 7) << 5) |
                             (((y * 3 + frame) & 7) << 2) | ((x ^ y ^ frame) & 3))
        return ((x + frame * 3) & 255, (y * 3 + frame) & 255, (x ^ y ^ frame) & 255)
    if source == 'lcd-fixture':
        frame = pattern['frame']
        require(type(frame) is int and 0 <= frame <= 2, 'invalid LCD fixture frame')
        if 'placement' in pattern:
            left, top, active_w, active_h = pattern['placement']
            require(all(type(v) is int for v in (left, top, active_w, active_h))
                    and 0 <= left and 0 <= top and active_w > 0 and active_h > 0
                    and left + active_w <= width and top + active_h <= height,
                    'LCD fixture placement exceeds canvas')
            if not (left <= x < left + active_w and top <= y < top + active_h):
                return (0, 0, 0)
            x, y = x - left, y - top
        return expand565((((x + frame * 3) & 31) << 11) |
                         (((y * 3 + frame) & 63) << 5) | ((x ^ y ^ frame) & 31))
    scene_w, scene_h = pattern.get('scene', [width, height])
    dimensions(scene_w, scene_h)
    offset_x, offset_y, window_w, window_h = pattern.get('crop', [0, 0, scene_w, scene_h])
    require(all(type(v) is int for v in (offset_x, offset_y, window_w, window_h))
            and 0 <= offset_x and 0 <= offset_y and window_w > 0 and window_h > 0
            and offset_x + window_w <= scene_w and offset_y + window_h <= scene_h,
            'source crop exceeds independently specified scene')
    if pattern.get('mirror', False):
        x = width - 1 - x
    if pattern.get('flip', False):
        y = height - 1 - y
    x, y = offset_x + x * window_w // width, offset_y + y * window_h // height
    if source == 'color-bars':
        return BARS[8 * x // scene_w]
    if source == 'testimage':
        require(scene_w > 1 and scene_h > 1, 'testimage scene must exceed one pixel per axis')
        return (255 * x // (scene_w - 1), 255 * y // (scene_h - 1),
                255 if ((x // 8) ^ (y // 8)) & 1 else 0)
    raise ReferenceError(f'unsupported independently specified pattern: {source}')


def expected_rgb(pattern, width, height, quantize565=False):
    dimensions(width, height)
    out = bytearray(width * height * 3)
    for y in range(height):
        for x in range(width):
            rgb = pattern_pixel(pattern, x, y, width, height)
            if quantize565:
                rgb = expand565(rgb565(rgb))
            at = 3 * (y * width + x)
            out[at:at + 3] = bytes(rgb)
    return bytes(out)


def expected565(pattern, width, height, low_byte_first=False):
    dimensions(width, height)
    out = bytearray(width * height * 2)
    order = '<H' if low_byte_first else '>H'
    for y in range(height):
        for x in range(width):
            struct.pack_into(order, out, 2 * (y * width + x),
                             rgb565(pattern_pixel(pattern, x, y, width, height)))
    return bytes(out)


def expected332(pattern, width, height):
    dimensions(width, height)
    return bytes(rgb332(pattern_pixel(pattern, x, y, width, height))
                 for y in range(height) for x in range(width))


def decode565(payload, width, height, low_byte_first=False):
    dimensions(width, height)
    require(len(payload) == 2 * width * height, 'RGB565 byte count differs from dimensions')
    order = '<H' if low_byte_first else '>H'
    return bytes(c for (word,) in struct.iter_unpack(order, payload) for c in expand565(word))


def limited601(rgb):
    r, g, b = rgb
    return tuple(max(0, min(255, v)) for v in (
        16 + (66 * r + 129 * g + 25 * b + 128) // 256,
        128 + (-38 * r - 74 * g + 112 * b + 128) // 256,
        128 + (112 * r - 94 * g - 18 * b + 128) // 256))


def expected422(pattern, width, height, order):
    dimensions(width, height)
    require(width % 2 == 0, 'YUV422 requires an even line width')
    require(order in ('YUYV', 'YVYU', 'UYVY', 'VYUY'), 'invalid YUV422 byte ordering')
    out = bytearray(width * height * 2)
    for y in range(height):
        for x in range(0, width, 2):
            y0, u0, v0 = limited601(pattern_pixel(pattern, x, y, width, height))
            y1, u1, v1 = limited601(pattern_pixel(pattern, x + 1, y, width, height))
            u, v = (u0 + u1 + 1) // 2, (v0 + v1 + 1) // 2
            if order == 'YUYV':
                quad = (y0, u, y1, v)
            elif order == 'YVYU':
                quad = (y0, v, y1, u)
            elif order == 'UYVY':
                quad = (u, y0, v, y1)
            else:
                quad = (v, y0, u, y1)
            at = 2 * (y * width + x)
            out[at:at + 4] = bytes(quad)
    return bytes(out)


def jpeg_dimensions(payload):
    """Parse T.81 markers before allocating in the optional external decoder.

    Require baseline sequential, eight-bit, three-component JPEG with a single
    scan; reject malformed lengths, trailing bytes and an absent EOI. Entropy
    bytes are traversed separately so stuffed FF00 is never treated as a marker.
    """
    require(4 <= len(payload) <= MAX_PAYLOAD, 'JPEG payload size outside bounds')
    require(payload[:2] == b'\xff\xd8', 'JPEG SOI missing')
    pos, shape, scan = 2, None, False
    components = None
    while pos < len(payload):
        require(payload[pos] == 255, f'JPEG marker prefix missing at {pos}')
        while pos < len(payload) and payload[pos] == 255:
            pos += 1
        require(pos < len(payload), 'truncated JPEG marker')
        marker = payload[pos]
        pos += 1
        if marker == 0xD9:
            require(scan and shape is not None and pos == len(payload), 'JPEG EOI/trailing bytes invalid')
            return shape
        require(marker not in (0, 0xD8) and not 0xD0 <= marker <= 0xD7,
                'unexpected standalone JPEG marker')
        require(pos + 2 <= len(payload), 'truncated JPEG segment length')
        length = int.from_bytes(payload[pos:pos + 2], 'big')
        require(length >= 2 and pos + length <= len(payload), 'JPEG segment outside payload')
        body = payload[pos + 2:pos + length]
        pos += length
        if marker == 0xC0:
            require(shape is None and len(body) == 15 and body[0] == 8 and body[5] == 3,
                    'JPEG must have one eight-bit, three-component SOF0')
            height, width = struct.unpack('>HH', body[1:5])
            dimensions(width, height)
            components = set(body[6::3])
            require(len(components) == 3, 'duplicate JPEG component identifiers')
            require(all(value in (0x11, 0x21, 0x22) for value in body[7::3]),
                    'unsupported JPEG sampling factors')
            shape = width, height
        elif 0xC1 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            raise ReferenceError('JPEG is not baseline sequential SOF0')
        elif marker == 0xDA:
            require(not scan and shape is not None and len(body) == 10 and body[0] == 3
                    and body[-3:] == b'\x00\x3f\x00', 'JPEG baseline scan invalid')
            require(set(body[1:7:2]) == components,
                    'JPEG scan component identifiers differ from frame')
            require(all(value >> 4 <= 1 and value & 15 <= 1 for value in body[2:7:2]),
                    'JPEG baseline Huffman table selector outside 0..1')
            scan = True
            while pos < len(payload):
                if payload[pos] != 255:
                    pos += 1
                    continue
                start = pos
                while pos < len(payload) and payload[pos] == 255:
                    pos += 1
                require(pos < len(payload), 'truncated JPEG entropy marker')
                if payload[pos] == 0 or 0xD0 <= payload[pos] <= 0xD7:
                    pos += 1
                    continue
                pos = start
                break
    raise ReferenceError('JPEG EOI missing')


def compare_jpeg(payload, width, height, pattern, max_error, mean_error):
    dimensions(width, height)
    require(jpeg_dimensions(payload) == (width, height), 'JPEG dimensions differ from expected')
    require(0 <= max_error <= 64 and 0 <= mean_error <= 16,
            'JPEG tolerance exceeds qualification bounds')
    try:
        from PIL import Image
    except ImportError as exc:
        raise ReferenceError('JPEG requires explicit Pillow dependency; install reference/requirements.txt') from exc
    try:
        with Image.open(io.BytesIO(payload)) as image:
            require(image.format == 'JPEG' and image.size == (width, height), 'decoder JPEG shape mismatch')
            image.load()
            actual = image.convert('RGB').tobytes()
    except (OSError, ValueError) as exc:
        raise ReferenceError(f'independent JPEG decoder rejected image: {exc}') from exc
    expected = expected_rgb(pattern, width, height)
    total, peak = 0, 0
    for got, want in zip(actual, expected):
        error = abs(got - want)
        total += error
        peak = max(peak, error)
    mean = total / len(expected)
    require(peak <= max_error and mean <= mean_error,
            f'JPEG color error max={peak}, mean={mean:.6f} exceeds {max_error}/{mean_error}')
    return actual, {'max_channel_error': peak, 'mean_channel_error': mean,
                    'max_channel_error_limit': max_error, 'mean_channel_error_limit': mean_error}
