# GUI ESP32-S3 Simulator Architecture

## Target stack
- Linux / Windows
- Qt6 Widgets frontend
- QEMU ESP32-S3 backend runtime

---

## UI Architecture

### Main Window Tabs
1. **Serial** — UART0 console with full serial-port settings
2. **Processor Status** — PC, registers, memory inspect, live/step/breakpoint controls
3. **Control** — Reset, boot mode, firmware loading, SPI flash, PSRAM, MAC, chip revision
4. **Debug** — GDB enable/disable, port, wait-for-attach, one-click launch
5. **Peripherals** — Dynamic per-device panel tabs with pin wiring diagrams

### Peripherals Tab — Device Panel System
The Peripherals tab uses a **per-device panel architecture**: each loaded
peripheral device gets its own dedicated tab inside the Peripherals view.
Panels are created dynamically by a factory based on device-declared
capabilities (`get_capabilities.panel`), so each virtual device owns its
subpanel definition.

```
PeripheralsWidget
├── Config toolbar (Browse / Load / Start all / Stop all / Refresh)
├── Device count label
├── QTabWidget (one tab per device)
│   ├── [📺 screen0] → DisplayPanel (ScrollArea)
│   │   ├── PinConnectionWidget (I²C bus, GPIO8-SDA ↔ SSD1306, GPIO9-SCL ↔ SSD1306)
│   │   ├── Screen Output (live framebuffer rendering, zoom 1-8x)
│   │   ├── Display Controls (on/off, invert, contrast slider)
│   │   └── Device Log
│   ├── [🌡 temp0] → SensorPanel (ScrollArea)
│   │   ├── PinConnectionWidget (I²C bus, GPIO8-SDA ↔ SHT21, GPIO9-SCL ↔ SHT21)
│   │   ├── Live Telemetry (large font: temperature °C, humidity %RH)
│   │   ├── Sensor Controls (spin boxes, progress bars, mode selector)
│   │   ├── CSV Playback (start/pause/stop, index counter)
│   │   └── Device Log
│   ├── [💾 spiflash0] → SpiFlashPanel (ScrollArea)
│   │   ├── PinConnectionWidget (SPI bus, GPIO10-CS → W25Q, GPIO11-MOSI → ...)
│   │   ├── Flash Info (size, R/W/erase stats)
│   │   ├── Memory Contents (hex viewer with address navigation)
│   │   └── Device Log
│   └── [🔌 uart-loop0] → UartDevicePanel (ScrollArea)
│       ├── PinConnectionWidget (UART bus, GPIO17-TX → LOOPBACK, GPIO18-RX ← LOOPBACK)
│       ├── UART Configuration (115200 8N1)
│       ├── Traffic Monitor (scrolling TX/RX log with send bar)
│       └── Device Log
└── Global log (manager messages + all device output)
```

### Pin Connection Widget
Every device panel includes a **PinConnectionWidget** at the top that shows:
- Bus type and controller (e.g. "I²C bus I2C0 addr: 0x3c")
- Each signal's GPIO wiring: `ESP32-S3 │ GPIO8 │ ↔ │ SDA │ SSD1306`
- Direction arrows: → (output from ESP32), ← (input to ESP32), ↔ (bidirectional)
- Electrical info: VIO voltage, pull-up resistance
- Bus speed: clock frequency, baud rate, SPI mode

### Device Panel Types

| Panel Class | Device Types | Key Features |
|-------------|-------------|--------------|
| `DisplayPanel` | SSD1306, SH1106, ST7789, ILI9341, any OLED/LCD | Live screen rendering from framebuffer data, contrast/invert controls, zoom |
| `SensorPanel` | SHT21, BME280, DHT22, MPU6050, any sensor | Real-time telemetry display, manual value injection, CSV playback |
| `SpiFlashPanel` | W25Q, AT25, any SPI NOR flash, EEPROM | Hex viewer, read/write stats, address navigation |
| `UartDevicePanel` | UART loopback, GPS, modem, any UART device | Traffic monitor, TX/RX display, send bar |
| `GenericDevicePanel` | Any unrecognized type (fallback) | Raw JSON state/capabilities viewer |

### Schema-driven panel contract (device-owned)
Each simulator can define UI directly in `get_capabilities`:

```json
{
   "panel": {
      "kind": "sensor",
      "title": "SHT21 Sensor",
      "description": "Temperature and humidity source.",
      "controls": [
         {
            "name": "temperature",
            "label": "Temperature",
            "type": "float",
            "unit": "°C",
            "min": -40,
            "max": 125,
            "section": "Environment",
            "description": "Injected ambient temperature.",
            "writable": true
         },
         {
            "name": "reset_counters",
            "label": "Reset Counters",
            "type": "action",
            "rpc_method": "reset_counters",
            "section": "Operations"
         }
      ]
   }
}
```

Supported control types in schema panel:
- `bool`, `int`, `float`/`double`, `enum`, `string`, `action`
- Optional keys: `label`, `description`, `section`, `unit`, `min`, `max`, `step`, `decimals`, `placeholder`, `writable`, `rpc_method`, `rpc_params`
- Optional dynamic view blocks:
   - `display` (state-key driven framebuffer render)
   - `metrics` (live card values from `state_path`)
   - `scripts` (read-only text/JSON panes from `state_path`)

### Device Panel Factory
`DevicePanelFactory::createPanel()` now selects panels by device-owned capabilities:
1. If `capabilities.panel` exists → `SchemaDevicePanel`
2. Otherwise → `GenericDevicePanel` fallback

---

## Runtime split
- **Frontend (Qt)**: view and user interaction — all widgets
- **Backend controller**: QEMU process control + QMP/monitor integration
- **Peripheral Manager**: JSON config loading, process lifecycle, JSON-RPC bridge
- **Device Simulators**: external Python processes (SSD1306, SHT21, SPI flash, UART loopback)

## Peripheral Simulator Protocol
- Transport: JSON-RPC 2.0 over stdio (one JSON object per line)
- Core methods: `get_capabilities`, `get_state`, `set_parameter`, `ping`
- Bus methods: `i2c_transfer`, `spi_transfer`, `uart_tx`, `uart_set_line`
- Notifications (sim → GUI): `telemetry`, `frame_update`, `uart_rx`, `playback_finished`
- Parameter changes from panel UI → `set_parameter` RPC → simulator process

## GDB support
- Codebase includes Xtensa/ESP32-S3 gdbstub
- Runtime options: `-s`, `-S`, `-gdb tcp::<port>`
- Debug tab provides one-click workflow with attach command

## Data flow: Panel ↔ Simulator

```
User adjusts slider in SensorPanel
  └→ parameterChangeRequested signal
     └→ PeripheralsWidget::onPanelParameterChange
        └→ PeripheralManager::setDeviceParameter
           └→ sendRpc("set_parameter", {"name":"temperature_c","value":30.0})
              └→ Python simulator process (stdin)
                 └→ Process returns JSON result (stdout)
                    └→ PeripheralManager::handleJsonMessage
                       └→ devicesChanged signal
                          └→ PeripheralsWidget::updateDevicePanels
                             └→ SensorPanel::updateState (UI refreshes)

Simulator periodically emits telemetry notification:
  └→ {"jsonrpc":"2.0","method":"telemetry","params":{"temperature_c":27.5}}
     └→ PeripheralManager::handleJsonMessage
        └→ device->state["telemetry"] = params
        └→ devicesChanged signal
           └→ SensorPanel::updateState (telemetry label updates)
```

## File structure (new panel system)

```
src/
├── MainWindow.h/cpp           # Main window with tab widget
├── SerialConsoleWidget.h/cpp  # UART0 serial console
├── CpuStatusWidget.h/cpp     # Processor status + debug controls
├── ControlPanelWidget.h/cpp   # Firmware/boot/flash/PSRAM controls
├── DebugWidget.h/cpp          # GDB integration tab
├── PeripheralsWidget.h/cpp    # Peripheral management + device tab container
├── DevicePanelBase.h/cpp      # Abstract base for all device panels
├── DevicePanelFactory.h/cpp   # Creates correct panel type from device config
├── PinConnectionWidget.h/cpp  # GPIO pin wiring diagram widget
├── DisplayPanel.h/cpp         # OLED/LCD screen panel (framebuffer renderer)
├── SensorPanel.h/cpp          # Sensor telemetry panel (temp, humidity, etc.)
├── SpiFlashPanel.h/cpp        # SPI flash hex viewer panel
├── UartDevicePanel.h/cpp      # UART device traffic panel
└── GenericDevicePanel.h/cpp   # Fallback raw-JSON panel

backend/
├── QemuController.h/cpp       # QEMU process + QMP + UART PTY
└── PeripheralManager.h/cpp    # Device process lifecycle + JSON-RPC bridge

device-sims/
├── jsonrpc_stdio.py           # JSON-RPC base server
├── ssd1306_sim.py             # SSD1306 OLED display simulator
├── sht21_sim.py               # SHT21 temp/humidity sensor simulator
├── spi_mem_sim.py             # SPI NOR flash simulator
└── uart_loopback_sim.py       # UART loopback simulator

peripherals/
├── peripherals.schema.json    # JSON schema for device config
└── peripherals.example.json   # Example config with 4 devices
```

## Next steps
- Route bus transactions between QEMU and external simulators via bridge signals
- Add color LCD panel support (RGB565, RGB888 pixel formats)
- Add accelerometer / IMU panel with 3D orientation visualization
- Add GPIO panel for bare pin toggling and LED simulation
- Support hot-plugging devices while QEMU is running
