#!/usr/bin/env python3
import json
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


def check_i2c():
    ssd = Ssd1306Simulator()
    res = ssd.handle(req("i2c_transfer", {
        "address": "0x3c",
        "ops": [{"dir": "write", "data": [0x00, 0xAF]}],
        "clock_hz": 400000,
    }))
    assert_true(res.get("ack") is True, "ssd1306 i2c ack failed")

    sht = Sht21Simulator(addr=0x40)
    write_res = sht.handle(req("i2c_transfer", {
        "address": "0x40",
        "ops": [{"dir": "write", "data": [0xE3]}, {"dir": "read", "len": 3}],
        "clock_hz": 100000,
        "repeated_start": True,
    }))
    assert_true(write_res.get("ack") is True, "sht21 i2c transfer failed")
    read_data = write_res.get("ops", [{}, {}])[1].get("data", [])
    assert_true(len(read_data) == 3, "sht21 i2c read length mismatch")


def check_spi():
    spi = SpiMemorySimulator(size_bytes=4096)
    jedec = spi.handle(req("spi_transfer", {"mode": "quad", "tx": [0x9F], "rx_len": 3}))
    assert_true(jedec.get("rx") == [0xEF, 0x40, 0x18], "spi jedec mismatch")

    spi.handle(req("spi_transfer", {"mode": "single", "tx": [0x06], "rx_len": 0}))
    spi.handle(req("spi_transfer", {"mode": "single", "tx": [0x02, 0x00, 0x00, 0x10, 0xAA, 0x55], "rx_len": 0}))
    read_back = spi.handle(req("spi_transfer", {"mode": "dual", "tx": [0x03, 0x00, 0x00, 0x10], "rx_len": 2}))
    assert_true(read_back.get("rx") == [0xAA, 0x55], "spi program/read mismatch")


def check_uart():
    uart = UartLoopbackSimulator(baud=115200)
    uart.handle(req("uart_set_line", {"baud": 921600, "data_bits": 8, "parity": "none", "stop_bits": 1}))
    tx = uart.handle(req("uart_tx", {"data": [0x48, 0x69]}))
    st = uart.handle(req("get_state"))
    assert_true(tx.get("ok") is True, "uart tx failed")
    assert_true(st.get("tx_count") == 2 and st.get("rx_count") == 2, "uart counters mismatch")


def check_example_config():
    cfg_path = Path(__file__).resolve().parent.parent / "peripherals" / "peripherals.example.json"
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    ids = {d.get("id") for d in cfg.get("devices", [])}
    assert_true({"screen0", "temp0", "spiflash0", "uart-loop0"}.issubset(ids), "example config missing expected devices")


def main():
    check_i2c()
    check_spi()
    check_uart()
    check_example_config()
    print("bridge smoke check: PASS")


if __name__ == "__main__":
    main()
