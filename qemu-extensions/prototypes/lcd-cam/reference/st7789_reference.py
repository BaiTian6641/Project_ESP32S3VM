#!/usr/bin/env python3
"""ST7789 8080-I transfer decoder, independent of the physical panel service.

Inputs are actual resolved WR rising-edge samples, not controller FIFO words.
Supported capture contract: 8/16-bit I80, RGB565; 8-bit I80 RGB666. Commands
outside the implemented capture lane fail closed rather than masquerade as
correct panel data. See provenance.json for datasheet sections.
"""
from image_reference import ReferenceError, dimensions, expand565, require


class St7789:
    PARAMS = {0x2A: 4, 0x2B: 4, 0x36: 1, 0x3A: 1}
    SIMPLE = {0x00, 0x10, 0x11, 0x13, 0x20, 0x21, 0x28, 0x29, 0x2C, 0x3C}

    def __init__(self, width, height, bus_width):
        dimensions(width, height)
        require(width <= 240 and height <= 320, 'ST7789 native RAM dimensions exceed 240x320')
        require(bus_width in (8, 16), 'I80 capture lane supports 8/16 bits only')
        self.width, self.height, self.bus_width = width, height, bus_width
        self.ram = bytearray(width * height * 3)
        self.frames = 0
        self.reset()

    def reset(self):
        self.window = [0, self.width - 1, 0, self.height - 1]
        self.madctl, self.colmod = 0, 0x66
        self.command, self.params, self.pixel = None, [], []
        self.x, self.y, self.written = 0, 0, 0
        self.first_pixel_ns = None
        self.sleep, self.display = True, False

    def _address(self):
        x, y = self.x, self.y
        if self.madctl & 0x20:
            x, y = y, x
        if self.madctl & 0x40:
            x = self.width - 1 - x
        if self.madctl & 0x80:
            y = self.height - 1 - y
        require(0 <= x < self.width and 0 <= y < self.height, 'MADCTL mapped pixel outside panel')
        return 3 * (y * self.width + x)

    def _advance(self):
        x0, x1, y0, y1 = self.window
        # Address-window coordinates are exchanged into native RAM by MV.
        self.x += 1
        if self.x > x1:
            self.x = x0
            self.y += 1
        if self.y > y1:
            self.y = y0
            return True
        return False

    def sample(self, timestamp_ns, dc, value, known_mask, cs=0, reset=1, rd=1):
        require(type(timestamp_ns) is int and timestamp_ns >= 0, 'invalid wire timestamp')
        require(dc in (0, 1) and cs in (0, 1) and reset in (0, 1) and rd in (0, 1),
                'unresolved I80 control signal')
        mask = (1 << self.bus_width) - 1
        require(type(value) is int and 0 <= value <= mask, 'wire sample exceeds panel bus width')
        require(known_mask == mask, 'disconnected/unknown I80 data bus at WR rising edge')
        if not reset:
            self.reset()
            return None
        if cs:
            return None
        require(rd == 1, 'simultaneous I80 read/write is invalid')
        if dc == 0:
            require(not self.params and not self.pixel, 'wrong DC interrupted parameters or partial pixel')
            require(self.command not in self.PARAMS, 'wrong DC interrupted required command parameters')
            command = value & 255
            require(command in self.PARAMS or command in self.SIMPLE or command == 1,
                    f'unsupported/wrong-bus ST7789 command 0x{command:02x}')
            self.command = command
            if command == 1:
                self.reset()  # Software reset leaves display RAM unaffected.
            elif command == 0x10:
                self.sleep = True
            elif command == 0x11:
                self.sleep = False
            elif command == 0x28:
                self.display = False
            elif command == 0x29:
                self.display = True
            elif command == 0x2C:
                self.x, self.y = self.window[0], self.window[2]
                self.written, self.first_pixel_ns = 0, None
            return None
        if self.command in self.PARAMS:
            self.params.append(value & 255)
            if len(self.params) == self.PARAMS[self.command]:
                if self.command in (0x2A, 0x2B):
                    start = (self.params[0] << 8) | self.params[1]
                    end = (self.params[2] << 8) | self.params[3]
                    axis = 0 if self.command == 0x2A else 2
                    limit = (self.height if self.madctl & 0x20 else self.width) if axis == 0 else (
                        self.width if self.madctl & 0x20 else self.height)
                    require(0 <= start <= end < limit, 'address window outside panel')
                    self.window[axis:axis + 2] = start, end
                elif self.command == 0x36:
                    require(self.params[0] & 3 == 0, 'reserved MADCTL bits set')
                    self.madctl = self.params[0]
                else:
                    require(self.params[0] & 7 in (5, 6), 'unsupported COLMOD capture format')
                    self.colmod = self.params[0]
                self.params.clear()
                self.command = None
            return None
        require(self.command in (0x2C, 0x3C), 'wrong DC: data without RAMWR or parameter command')
        require(not self.sleep and self.display, 'pixel write cannot qualify an asleep/blank display')
        if self.colmod & 7 == 5:
            if self.bus_width == 8:
                self.pixel.append(value)
                if len(self.pixel) != 2:
                    return None
                word = (self.pixel[0] << 8) | self.pixel[1]
            else:
                word = value
            rgb = expand565(word)
        else:
            require(self.bus_width == 8, 'RGB666 needs eight-bit bus in this capture lane')
            self.pixel.append(value)
            if len(self.pixel) != 3:
                return None
            rgb = tuple((v & 0xFC) | (v >> 6) for v in self.pixel)
        self.pixel.clear()
        if self.madctl & 8:
            rgb = rgb[::-1]
        at = self._address()
        self.ram[at:at + 3] = bytes(rgb)
        self.written += 1
        if self.first_pixel_ns is None:
            self.first_pixel_ns = timestamp_ns
        if self._advance():
            self.frames += 1
            result = {'sequence': self.frames - 1, 'start_ns': self.first_pixel_ns,
                      'end_ns': timestamp_ns, 'pixels': self.written, 'rgb': bytes(self.ram)}
            self.written, self.first_pixel_ns = 0, None
            return result
        return None

    def finish(self):
        require(not self.params and self.command not in self.PARAMS and not self.pixel and self.written == 0,
                'capture ended inside a command/pixel/address window')
