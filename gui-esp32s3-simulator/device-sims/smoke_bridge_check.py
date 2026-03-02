#!/usr/bin/env python3
"""Cross-platform virtual-device smoke and bus-behavior checks.

Covers Linux/Windows/macOS-safe simulator logic using in-process calls:
- I2C split-method and transfer-method behavior (SSD1306 + SHT21)
- SPI transfer behavior in single/dual/quad modes with WEL/program/read
- UART set-line + TX loopback path with tick-delivery verification
- Example peripheral config completeness
"""

import json
import os
import platform
import time
from pathlib import Path

from ssd1306_sim import Ssd1306Simulator
from sht21_sim import Sht21Simulator
from spi_mem_sim import SpiMemorySimulator
from uart_loopback_sim import UartLoopbackSimulator


def req(method, params=None):
    return type("Req", (), {"method": method, "params": params or {}})()


def assert_true(cond, msg):
    if not cond:
        raise AssertionError(msg)


def check_i2c_split_and_transfer():
    ssd = Ssd1306Simulator()

    split_write = ssd.handle(req("i2c_write", {"addr": "0x3c", "data": [0x00, 0xAF]}))
    assert_true(split_write.get("ack") is True, "ssd1306 split i2c_write failed")

    split_read = ssd.handle(req("i2c_read", {"addr": "0x3c", "len": 1}))
    assert_true(split_read.get("ack") is True, "ssd1306 split i2c_read failed")
    assert_true(split_read.get("data") == [0x00], "ssd1306 status read should indicate display ON")

    tx = ssd.handle(
        req(
            "i2c_transfer",
            {
                "address": "0x3c",
                "ops": [{"dir": "write", "data": [0x40, 0xAA, 0x55]}],
                "clock_hz": 400000,
                "repeated_start": False,
            },
        )
    )
    assert_true(tx.get("ack") is True, "ssd1306 transfer i2c failed")

    sht = Sht21Simulator()

    split_cmd = sht.handle(req("i2c_write", {"addr": "0x40", "data": [0xE3]}))
    assert_true(split_cmd.get("ack") is True, "sht21 split command write failed")

    split_meas = sht.handle(req("i2c_read", {"addr": "0x40", "len": 3}))
    assert_true(split_meas.get("ack") is True, "sht21 split read failed")
    assert_true(len(split_meas.get("data", [])) == 3, "sht21 split read length mismatch")

    transfer_meas = sht.handle(
        req(
            "i2c_transfer",
            {
                "address": "0x40",
                "ops": [{"dir": "write", "data": [0xE3]}, {"dir": "read", "len": 3}],
                "clock_hz": 100000,
                "repeated_start": True,
            },
        )
    )
    assert_true(transfer_meas.get("ack") is True, "sht21 transfer failed")
    transfer_ops = transfer_meas.get("ops", [])
    assert_true(len(transfer_ops) >= 2, "sht21 transfer ops incomplete")
    read_data = transfer_ops[1].get("data", [])
    assert_true(len(read_data) == 3, "sht21 transfer read length mismatch")


def check_spi_modes_and_program_readback():
    spi = SpiMemorySimulator(size_bytes=4096)

    jedec = spi.handle(req("spi_transfer", {"mode": "quad", "tx": [0x9F], "rx_len": 3}))
    assert_true(jedec.get("ok") is True, "spi jedec transfer failed")
    assert_true(jedec.get("rx") == [0xEF, 0x40, 0x18], "spi jedec mismatch")

    spi.handle(req("spi_transfer", {"mode": "single", "tx": [0x06], "rx_len": 0}))
    spi.handle(
        req(
            "spi_transfer",
            {"mode": "single", "tx": [0x02, 0x00, 0x00, 0x10, 0xAA, 0x55], "rx_len": 0},
        )
    )

    # Program operations are realistically asynchronous; poll WIP until clear.
    status_ok = False
    for _ in range(20):
        spi.tick()
        status = spi.handle(req("spi_transfer", {"mode": "single", "tx": [0x05], "rx_len": 1}))
        if status.get("ok") is True:
            status_ok = True
        sr1 = status.get("rx", [0x01])[0] if status.get("rx") else 0x01
        if (sr1 & 0x01) == 0:
            break
        time.sleep(0.005)
    assert_true(status_ok, "spi status read failed")

    read_back = spi.handle(
        req("spi_transfer", {"mode": "dual", "tx": [0x03, 0x00, 0x00, 0x10], "rx_len": 2})
    )
    assert_true(read_back.get("ok") is True, "spi readback transfer failed")
    assert_true(read_back.get("rx") == [0xAA, 0x55], "spi program/read mismatch")


def check_uart_line_and_loopback():
    uart = UartLoopbackSimulator(baud=115200)

    set_line = uart.handle(
        req("uart_set_line", {"baud": 115200, "data_bits": 8, "parity": "none", "stop_bits": 1})
    )
    assert_true(set_line.get("ok") is True, "uart_set_line failed")

    tx = uart.handle(req("uart_tx", {"data": [0x48, 0x69]}))
    assert_true(tx.get("ok") is True, "uart tx failed")

    for _ in range(30):
        uart.tick()
        st = uart.handle(req("get_state"))
        counters = st.get("counters", {})
        if counters.get("rx") == 2:
            break
        time.sleep(0.005)

    st = uart.handle(req("get_state"))
    counters = st.get("counters", {})
    assert_true(counters.get("tx") == 2, "uart tx counter mismatch")
    assert_true(counters.get("rx") == 2, "uart rx counter mismatch")


def check_example_config():
    cfg_path = Path(__file__).resolve().parent.parent / "peripherals" / "peripherals.example.json"
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    ids = {d.get("id") for d in cfg.get("devices", [])}
    assert_true(
        {"screen0", "temp0", "spiflash0", "uart-loop0"}.issubset(ids),
        "example config missing expected devices",
    )


def main():
    check_i2c_split_and_transfer()
    check_spi_modes_and_program_readback()
    check_uart_line_and_loopback()
    check_example_config()

    print(
        "bridge smoke check: PASS"
        f" (os={platform.system()} py={platform.python_version()} pid={os.getpid()})"
    )


if __name__ == "__main__":
    main()
