# Radio prerequisites research (Wi-Fi calibration stall, BLE LP-clock assertion, BLE PIF fault)

Date: 2026-10-07. Researcher slice: radio-prerequisite researcher (RadioResearcher).
Repository HEAD at time of writing: `8cb306fe4709d6eb3a6e5f274f24349befecb21a` (branch `codex/simulator-foundation`).

Method and provenance for every RE claim below:

* ROM analysis used the pinned `esp-rom-elfs` `20241011` release
  (`~/.espressif/tools/esp-rom-elfs/20241011/esp32s3_rev0_rom.elf`,
  SHA256 `c0ce0f338d1de1bdc6efbef1591779a2a42c1ab7d759d3c6ae8ae63a7dd34cfd`)
  disassembled with `xtensa-esp-elf-objdump` from
  `~/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204` (GNU objdump for Xtensa).
* BLE controller blob analyzed from
  `build-idf-6.1/components/bt/controller/lib_esp32c3_family/esp32s3/libbtdm_app.a`
  (SHA256 `513488f088f7806c5a815580fd00e189265962a8824091b1e60618b7f345d0c1`),
  members `arch_main.o`, `rf_espressif.o`, `rwip.o`.
* Prior QEMU observations are quoted from the repo evidence docs
  (`docs/hardware-reference.md`, `docs/implementation-checkpoint.md`,
  `docs/handoff/06-radio-simd-hardware.md`); they were not re-run here.
* New physical ground truth (2026-10-07 rebaselined run, user-approved, read-only
  for this research): `build-hardware-host/run-2026-10-07-rebaselined/wifi/capture/records.json`
  and `.../ble/capture/records.json` (IDF `25fe69f946`, chip revision 2, both
  `complete: true, successful: true`).
* Line references use the working tree at the HEAD above.

Notation: `[INFERENCE]` marks a conclusion that is supported but not directly
observable; everything else is traceable to a cited file, capture, or URL.

---

## 1. Wi-Fi calibration stall — `0x6000e050[26:24]` poll in `ram_read_sar2_code`

### 1.1 Observed behavior (QEMU, both lanes)

Unmodified development-IDF Wi-Fi firmware completes NVS/netif/event-loop/Wi-Fi
init and mode selection, then never returns from `esp_wifi_start` (full RF
calibration) in official and recovered QEMU. CPU0 PC `0x40036a46`, caller
`0x42011772` (vendor `ram_read_sar2_code` in the flash-mapped Wi-Fi lib); the
ROM loop reads `0x6000e050`, extracts bits 26:24 and waits for value 7 after
writes `0x6000e060←0x00ff501a`, `0x6000e05c←0x0080016a`, `0x6000e05c←0x0088016a`.
Source: `docs/hardware-reference.md:254-263`, `docs/implementation-checkpoint.md:220-224`,
`docs/handoff/06-radio-simd-hardware.md:11-19`.

### 1.2 Peripheral ownership of `0x6000E000-0x6000EFFF`

None of the four candidate public blocks owns this window on ESP32-S3:

| Block | Base (pinned) | Verdict |
| --- | --- | --- |
| APB_SARADC | `0x60040000` (`build-idf-6.1/components/soc/esp32s3/register/soc/reg_base.h`, `DR_REG_APB_SARADC_BASE`) | No |
| SENS | `0x60008800` (`reg_base.h`, `DR_REG_SENS_BASE`) | No |
| RTC_CNTL | `0x60008000` (`reg_base.h`, `DR_REG_RTCCNTL_BASE`) | No |
| PWDET | No public block/header on S3 | No |

`reg_base.h` has no base address between `DR_REG_UHCI1_BASE 0x6000C000` and
`DR_REG_I2S_BASE 0x6000F000`: `0x6000D000`/`0x6000E000` are unnamed in public
S3 headers. The only public identifiers for the window are in
`build-idf-6.1/components/soc/esp32s3/include/soc/regi2c_defs.h`:

* `I2C_MST_ANA_CONF0_REG  0x6000E040` — `I2C_MST_BBPLL_STOP_FORCE_HIGH` BIT2,
  `I2C_MST_BBPLL_STOP_FORCE_LOW` BIT3, `I2C_MST_BBPLL_CAL_DONE` BIT24
* `ANA_CONFIG_REG  0x6000E044` — `ANA_CONFIG_S 8`, `ANA_CONFIG_M 0x3FF`,
  `I2C_BBPLL_M` BIT17 ("Clear to enable BBPLL"), `I2C_SAR_M` BIT18 ("Clear to
  enable SAR")
* `ANA_CONFIG2_REG 0x6000E048` — `ANA_SAR_CFG2_M` BIT16

So the owner is the **REGI2C / I2C_MST analog I2C master** (the same block the
recovered QEMU stubs at `0x6000E000`, size 0x100). The ESP32-S3 TRM does not
document it: the local TRM extract `docs/Esp32-s3_technical_reference_manual_en.md`
(TRM v1.2) contains no REGI2C/I2C_MST chapter, and Espressif's official
"ESP32-S3 Memory map" PDF (`https://dl.espressif.com/public/esp32s3-mm.pdf`,
fetched 2026-10-07) lists no peripheral between `UHCI1 0x6000C000` and
`I2S 0x6000F000`.

Public naming gap: offsets `0x50`, `0x5c`, `0x60` inside this block have **no
public names on ESP32-S3**. The closest public descendant is ESP32-C6's
`LP_I2C_ANA_MST` block
(`build-idf-6.1/components/soc/esp32c6/register/soc/lp_i2c_ana_mst_reg.h`):
`LP_I2C_ANA_MST_I2C0_CTRL_REG +0x00` (CTRL [24:0], `I2C0_BUSY` RO [25]),
`I2C0_CONF_REG +0x04` (CONF [23:0]) with `I2C0_STATUS` RO [31:24] **default 7**,
`I2C0_DATA_REG +0x08` (`RDATA` [7:0], `CLK_SEL` [10:8], `I2C_MST_SEL` [11]),
`ANA_CONF1 +0x0C`, `NOUSE +0x10`, `DEVICE_EN +0x14`, `DATE +0x3FC`. This shows
the block family exposes a hardware status field whose power-on value is `7`,
consistent with the ROM's "wait until 7" idle test.

### 1.3 ROM protocol decode (primary evidence)

From `esp32s3_rev0_rom.elf` disassembly:

* `rom_i2c_paral_write` (`0x40035684`): builds a transaction word
  `(func<<26) | BIT24 | (value<<16) | (reg<<8) | slave` into **`0x6000E000`**
  and a second word into **`0x6000E004`**, then spins with `memw` on
  `*0x6000E000 & 0x02000000` (BIT25 = engine busy) until clear.
* `rom_i2c_paral_read` (`0x40035614`): same encoding, engine address
  `0x6000E000` in its literal pool.
* `rom_i2c_paral_set_read` (`0x400355d8`): computes `~(mask_a | mask_b)` from
  `rom_phyFuns` callbacks and writes **`0x6000E044`** — "clear bit to enable
  slave", exactly matching `I2C_BBPLL_M`/`I2C_SAR_M` in `regi2c_defs.h`.
* `rom_get_i2c_hostid` (`0x400354bc`) / `rom_i2c_paral_set_mst0` (`0x40035594`):
  read-modify-write **`0x6000E048`** (`ANA_CONFIG2_REG`).

The stalled calibration sequence itself:

* `rom_pwdet_sar2_init` (`0x40036470`):
  * `SENS_SAR_MEAS2_MUX_REG (0x60008834)` — clear `SENS_SAR2_RTC_FORCE` BIT31
    (`build-idf-6.1/components/soc/esp32s3/register/soc/sens_reg.h:377-380`)
  * `SENS_SAR_MEAS2_CTRL2_REG (0x60008830)` — set `SENS_SAR2_EN_PAD_FORCE`
    BIT31, then clear bits [30:15] (mask `0x8007ffff`) (`sens_reg.h:339-342`)
  * `0x6000e060` — set bits {3,4}, clear bit 5 (OR `0x18`, AND `~0x20`)
  * `0x6000e05c` — `(*reg & 0xffff0000) | 0x016a` (low half = `0x01`,`0x6a`)
* `rom_pkdet_vol_start` (`0x40036a18`):
  * `0x6000e05c |= 0x00800000` (BIT23) → observed value `0x0080016a`
  * `0x6000e05c |= 0x00080000` (BIT19) → observed value `0x0088016a`
  * poll loop at `0x40036a41`: `memw; l32i` from `0x6000e050`;
    `extui a8, a8, 24, 3` (**bits [26:24]**); `bnei a8, 7` → loop.
    **PC `0x40036a46` is exactly this `extui`** — the captured stall PC.
  * after the poll: clear bit 1 then set bit 1 of `0x6000e050`.

Call chain (ROM): `rom_read_sar2_code` (`0x40036ac8`, public name in
`build-idf-6.1/components/esp_rom/esp32s3/ld/esp32s3.rom.ld:1317`
`rom_read_sar2_code = 0x400060e4` thunk; body `0x40036ac8`) loops 4×:
call `rom_phyFuns` table entry 72 = `rom_pkdet_vol_start` (`0x40036a18`),
entry 73 = `rom_read_sar_dout` (`0x40036aa4`), sum the returned 16-bit codes,
return `sum >> 2` (4-sample average). Table pointer verified: `*(0x3fcef3d4) =
0x3fcef3d8`, `*(0x3fcef4f8) = 0x40036a18`, `*(0x3fcef4fc) = 0x40036aa4`.
Related public symbols: `rom_pwdet_sar2_init`, `rom_en_pwdet`, `rom_i2c_sar2_init_code`,
`rom_get_i2c_mst0_mask`, `rom_get_i2c_hostid`, `rom_i2cmst_reg_init`
(`esp32s3.rom.ld:1271,1317,1364` and `esp32s3_rev0_rom.elf` symbol table).

Interpretation: this is the **PWDET (RF power detector) SAR2 measurement** used
during full PHY calibration; `0x6000e050[26:24]` is a 3-bit status of the
REGI2C/SAR-PWDET flow that must reach `7` (idle/done pattern; cf. C6
`I2C0_STATUS` default 7) before the ROM accepts the measurement.

### 1.4 Hardware vs QEMU divergence

Hardware (new ground truth, `run-2026-10-07-rebaselined/wifi/capture/records.json`):
stages `nvs_flash_init`, `esp_netif_init`, `esp_event_loop_create_default`,
`esp_wifi_init`, `esp_wifi_set_mode`, `esp_wifi_start`, `esp_wifi_stop`,
`esp_wifi_deinit` — all attempted and `err=0`, `last_stage: esp_wifi_deinit`,
`successful: true`. Real silicon completes the full RF calibration that QEMU
never finishes.

Recovered QEMU: `hw/misc/esp32s3_regi2c.c` decodes only `A_REGI2C_ANA_CONF0`
(reads `| REGI2C_BBPLL_CAL_DONE`), `A_REGI2C_ANA_CONFIG`, `A_REGI2C_ANA_CONFIG2`
(lines 34-44); offsets `0x50/0x5c/0x60` fall into a plain register file that
reads back 0 (lines 45-52). Therefore `[26:24]` never reaches 7 and the ROM
loops forever. The mapped block is at `ESP32S3_REGI2C_BASE`
(`qemu/hw/xtensa/esp32s3.c:510-515`).

### 1.5 What hardware behavior must be modeled (observed requirements)

Facts the model must reproduce, from the decodes above; this is a list of
observed dependencies, not an implementation plan:

1. REGI2C analog-I2C engines with serialized transactions: writes to the
   engine registers start a transaction; a busy flag (`0x6000E000` BIT25 for
   engine 0) clears when finished.
2. A status field at `0x6000e050[26:24]` that transitions to `7` only after the
   SAR2/PWDET sequence triggered by `0x6000e05c/0x6000e060` completes —
   explicitly **not** an unconditional `7` (repeated repo instruction:
   `docs/implementation-checkpoint.md:223-224`, `docs/hardware-reference.md:272-276`).
3. SAR ADC2 RTC-mode control path: `SENS_SAR_MEAS2_CTRL2`/`SENS_SAR_MEAS2_MUX`
   force bits must gate an (at least timed) SAR2 conversion whose result
   `rom_read_sar_dout` returns, four times, non-degenerate, since the caller
   averages them.
4. Clock/reset ownership: the analog I2C engines and SAR/PWDET are clocked
   peripherals; the ROM's `rom_i2cmst_reg_init` (`0x400389f0`) programs the
   master (source of the observed `0x6000e060` base value `0x00ff5012` →
   `0x00ff501a` after `|0x18`). Any clock-gating model must leave these running
   or progress the engines accordingly.

---

## 2. BLE low-power-clock assertion (`select_src_ret && set_div_ret`, bt.c:1757)

### 2.1 Pinned source (development checkout)

`build-idf-6.1/components/bt/controller/esp32c3/bt.c` — this is the controller
source compiled for ESP32-S3 (the S3 BLE controller library lives in
`components/bt/controller/lib_esp32c3_family/esp32s3/`; `components/bt/controller/esp32s3/`
contains only `Kconfig.in`).

* `btdm_low_power_mode_init` at `bt.c:1668`.
* `s_lp_cntl.lpclk_sel` default path: `bt.c:1682` (`cfg->sleep_clock`;
  falls back to `ESP_BT_SLEEP_CLOCK_MAIN_XTAL` when sleep mode is not 1).
* Main-XTAL branch `bt.c:1749-1759`:
  `select_src_ret = btdm_lpclk_select_src(BTDM_LPCLK_SEL_XTAL);`
  `set_div_ret = btdm_lpclk_set_div(esp_clk_xtal_freq() / MHZ);`
  `assert(select_src_ret && set_div_ret);` — **line 1757**, the observed
  assertion (40 MHz crystal ⇒ divider value 40).
* RTC-slow/32K branch for contrast: `bt.c:1760-1769`
  (`btdm_lpclk_select_src(BTDM_LPCLK_SEL_RTC_SLOW)`, `set_div(0)`).
* The helpers are extern blob symbols: `bt.c:283-285`.

### 2.2 What the helper functions actually do (blob decode)

`libbtdm_app.a:arch_main.o` (`btdm_lpclk_select_src` at object offset `0xc`,
`btdm_lpclk_set_div` at `0x4`):

* `btdm_lpclk_select_src(sel)` (sel < 4): `addx4` indexes a 4-entry table,
  then `memw` read-modify-write of **`0x600C002C`**: field `[23:12]`
  (mask `0xfff000`) ← `table[sel]`; then reads the register back and returns
  `((readback & table[sel]) == table[sel])` — **write-then-verify**.
* `btdm_lpclk_set_div(div)`: returns false if `div & ~0xfff`; otherwise `memw`
  read-modify-write of **`0x600C0028`** `[11:0]` ← `div` (with readback in the
  function tail).

### 2.3 Register names (pinned header)

`build-idf-6.1/components/soc/esp32s3/register/soc/system_reg.h:629-679`:

* `SYSTEM_BT_LPCK_DIV_INT_REG = SYSTEM_BASE(0x600C0000) + 0x28` —
  `SYSTEM_BT_LPCK_DIV_NUM [11:0]`, default `0xFFF`... header comment says
  default `12'd255`.
* `SYSTEM_BT_LPCK_DIV_FRAC_REG = + 0x2C` — `SYSTEM_LPCLK_RTC_EN [28]`,
  `SYSTEM_LPCLK_SEL_XTAL32K [27]`, `SYSTEM_LPCLK_SEL_XTAL [26]`,
  `SYSTEM_LPCLK_SEL_8M [25]` (default 1), `SYSTEM_LPCLK_SEL_RTC_SLOW [24]`,
  `SYSTEM_BT_LPCK_DIV_A [23:12]`, `SYSTEM_BT_LPCK_DIV_B [11:0]`.

So the BLE LP-clock configuration goes through **SYSTEM (LPCLK divider)
registers, not RTC_CNTL**, and the select function verifies its write by
readback of `BT_LPCK_DIV_A [23:12]`. `[INFERENCE]` the 4-entry table holds the
`DIV_A`/source encoding per `ESP_BT_SLEEP_CLOCK_*` selection; exact table
values were not extracted.

### 2.4 Why official QEMU fails / recovered QEMU passes

* Official QEMU (esp-develop-9.2.2 base): the assertion fires — the readback of
  `0x600C002C [23:12]` does not return the written value, so
  `btdm_lpclk_select_src` returns false
  (`docs/hardware-reference.md:255`, `docs/handoff/06-radio-simd-hardware.md:25-26`).
* Recovered core: `qemu/hw/xtensa/esp32s3_clk.c` implements
  `A_SYSTEM_BT_LPCK_DIV_INT`/`A_SYSTEM_BT_LPCK_DIV_FRAC` as plain storage on
  both read (lines 76-81) and write (lines 165-170), so the write-then-verify
  succeeds and the controller proceeds — consistent with the recovered lane
  getting past this point to the PIF fault.
* Hardware (`run-2026-10-07-rebaselined/ble/capture/records.json`): `nvs`,
  `esp_bt_controller_init`, `esp_bt_controller_enable`, `esp_bluedroid_init`,
  `esp_bluedroid_enable`, disable/deinit all `err=0`; the same code passes
  line 1757 on silicon rev 0.2.

### 2.5 Clock model requirements (observed)

1. `0x600C0028`/`0x600C002C` must store their fields and return them on read
   (write-then-verify contract proven by the blob disassembly).
2. Beyond the assert, the LP clock is *used*: `btdm_lpcycle_us` is derived from
   `RTC_CLK_CAL_FRACT` / `esp_clk_slowclk_cal_get()` (`bt.c:1758-1769`), and the
   32K/RTC-slow alternatives check `rtc_clk_slow_src_get()` against
   `SOC_RTC_SLOW_CLK_SRC_XTAL32K` / `SOC_RTC_SLOW_CLK_SRC_RC_SLOW`
   (`bt.c:1719-1737`). A faithful model needs the selected slow-clock source
   and its calibration value to be observable, not just the two SYSTEM
   registers.
3. RC32K/XTAL32K startup and calibration are touched by the slow-clock path
   (`bt.c:1760-1764` comment); their register-level behavior was **not**
   exercised by the observed MAIN_XTAL failure and is not characterized here.

---

## 3. BLE PIF fault — `LoadStorePIFAddrError`, PC `0x40056f60`, EXCVADDR `0x3fc00100`

### 3.1 What is at `0x3fc00100`

Nothing that any public ESP32-S3 document maps:

* Espressif official "ESP32-S3 Memory map" PDF
  (`https://dl.espressif.com/public/esp32s3-mm.pdf`, fetched 2026-10-07):
  data-bus internal memory is ROM0 `0x3FF00000`, RTC FAST 8 KB `0x600FE000`,
  SRAM0/SRAM1 from `0x3FC88000`, SRAM2 `0x3FCF0000`. No `0x3FC00000` region.
* IDF linker view
  (`build-idf-6.1/components/esp_system/ld/esp32s3/memory.ld.in`):
  `SRAM_DRAM_START 0x3FC88000`; RTC FAST memory `0x600FE000-0x60100000` (8 KB,
  "shares the same address range for both data and instructions"); RTC SLOW
  `0x50000000` (8 KB).
* ESP32-S3 TRM (v1.2) chapter 4 address-mapping sections are not present in the
  local md extract (TOC/tables only), but the official map PDF above is the
  same v1.2-era layout.

`[INFERENCE]` `0x3FC00000-0x3FC1FFFF` is an **undocumented modem/BT RAM window**
(the RivieraWaves "EM" — exchange/environment memory region used by the BLE
controller), present on real silicon but omitted from public maps. Evidence:
the BLE controller blob actively computes addresses there (below), and the
same firmware completes controller init/enable on real silicon
(`run-2026-10-07-rebaselined/ble/capture/records.json`, chip revision 2).

### 3.2 Who touches it (blob decode, `libbtdm_app.a`)

* `rf_espressif.o` `r_rw_rf_init` (the frame named in the QEMU trace):
  * reads a function table via `r_modules_funcs_p` (`0x3c0`, `0x210`, `0x214`,
    `0x218`, `0x21c`, `0x220`, `0x224`, `0x228`, `0x1e4`, `0x1e8` entries) and
    calls several entries;
  * performs `memw` read-modify-write of **`0x60031078`**: keep `[31:16]`,
    set `[15:0] ← 0x0100` (literal pool word `0x60031078`, no relocation).
* `rf_espressif.o` `r_rf_em_init`: builds a 40-byte table on the stack, calls
  `r_plf_funcs_p` entry at offset `188` with argument `0x100`, then
  `memcpy(<returned pointer>, table, 0x28)` — a platform callback invoked with
  offset `0x100` whose result is a destination pointer.
* `rf_espressif.o` `r_rf_sleep` / `r_rf_sleep_hack`: `memw` RMW of
  **`0x60042000`** (set/clear bits `0x47` / `0x10`).
* `rwip.o` `r_rwip_init`: wires `r_modules_funcs_p`, `rwip_param`,
  `btdm_env_p`, `rwip_rf`, `r_plf_funcs_p`, `r_ip_funcs_p`, `r_hli_funcs_p`,
  `r_osi_funcs_p` etc.; `arch_main.o` holds `rw_pre_main` (`0x28`) and
  `btdm_controller_task` (`0x5c`) — matching the observed call-chain names.
* `r_rf_reg_rd`/`r_rf_reg_wr` in the blob are empty stubs; ROM
  `r_rf_reg_rd/wr` (`0x4002af54/0x4002af5c`) likewise — real RF access is the
  `memw` window above, not these.

`PC 0x40056f60` is not a BT function: it is **ROM `memcpy + 0x1c`**
(`memcpy = 0x40056f44`, `esp32s3_rev0_rom.elf` symbol table; next symbol
`memmove = 0x4005703c`). The fault happens inside the copy whose destination
(the load/store address) is `0x3fc00100` = implied base `0x3FC00000` + offset
`0x100` — the exact argument `0x100` passed through the platform callback in
`r_rf_em_init`. `[INFERENCE]` `r_plf_funcs_p` entry 188/4=47 is the ROM/Blob
"EM address of offset" accessor returning `0x3FC00000 + offset` on silicon.

### 3.3 Why QEMU raises EXCCAUSE 0x0f

* Recovered-core memory map `qemu/hw/xtensa/esp32s3.c:117-128`: DRAM
  `0x3FC80000` size `0x170000`; RTCFAST `0x600fe000` size `0x2000`; nothing at
  `0x3FC00000`.
* The catch-all `esp32s3.iomem` covers `0x60000000` size `0xd1000`
  (`esp32s3.c:816-818`, `ESP32S3_IO_START_ADDR = DR_REG_UART_BASE`,
  `qemu/include/hw/misc/esp32s3_reg.h:82`) and returns 0 for unsupported reads
  with warnings disabled (`esp32s3.c:534-546`, `ESP32S3_IO_WARNING 0` at
  `esp32s3.c:138`). That is why the `0x60031078`/`0x60042000` accesses do **not**
  fault while the `0x3FC00100` access does.
* Xtensa transaction-failure path: `qemu/target/xtensa/helper.c:306-315`
  `xtensa_cpu_do_transaction_failed` raises `LOAD_STORE_PIF_ADDR_ERROR_CAUSE`
  (`qemu/target/xtensa/cpu.h:280`, EXCCAUSE 15 = 0x0f) with the faulting vaddr —
  exactly the observed `LoadStorePIFAddrError / EXCVADDR 0x3fc00100 /
  EXCCAUSE 0x0f` (`docs/hardware-reference.md:283-286`).

### 3.4 What the blob expects (observed dependencies)

1. A RAM window at `0x3FC00000+` (at least offset `0x100..0x128` exercised by
   the 40-byte copy; full extent unknown) that accepts stores and returns them.
2. `memw`-guarded RF register window `0x60031078` (RMW with upper-half
   retention) and `0x60042000` — the current catch-all returning 0 lets the
   code run but is not a faithful model (values written are lost).
3. The platform-function indirection (`r_plf_funcs_p` → table at
   `0x3fceff5c` area, `.bss.interface.bluetooth`, runtime-initialized from ROM
   data) must resolve entry 188 to something consistent with the EM window.
4. ROM BT data lives in data SRAM, e.g. `.data_btdm` at `0x3fcef174`
   (`esp32s3_rev0_rom.elf` section table) and
   `btdm_controller_rom_data_init` (`0x40006f10`) memcpy's 12 bytes from
   `*(0x40057350)` (`_data_start_btdm_rom`, ROM word value `0x40057d28`) into
   `0x3fcef174` — this part already works in QEMU (ROM data area is inside the
   modeled DRAM).

---

## 4. Prior art (web, fetched 2026-10-07)

Confirmed:

* **Espressif's official QEMU support matrix** — `esp-toolchain-docs`
  `qemu/README.md` (`https://github.com/espressif/esp-toolchain-docs/blob/main/qemu/README.md`):
  feature table lists **Wi-Fi ❌ and Bluetooth ❌ for ESP32, ESP32-S3 and
  ESP32-C3** ("The Ethernet controller can be used for networking instead"),
  plus "At the moment, Espressif does not provide support for QEMU." The IDF
  QEMU guide (`https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/tools/qemu.html`)
  mirrors the fork's supported-peripheral framing.
* **Maintainer statement** — esp-idf issue #15087
  (`https://github.com/espressif/esp-idf/issues/15087`, retrieved 2026-10-07;
  search index marks it ~21 months old): "wifi is not supported in our QEMU
  fork"; recommends the OpenCores Ethernet MAC option for emulated networking.
* **esp32-open-mac** (`https://github.com/esp32-open-mac/esp32-open-mac`,
  and their QEMU fork `https://github.com/esp32-open-mac/qemu`, RIOT Summit 2024
  talk PDF `http://summit.riot-os.org/2024/wp-content/uploads/sites/19/2024/09/04-1-Jasper_Devreker.pdf`):
  reverse-engineered Wi-Fi MAC/driver work for the **original ESP32**,
  including a QEMU fork used to reverse the Wi-Fi hardware. No ESP32-S3 radio
  model published.
* **Ebiroll/qemu_esp32** (`https://github.com/Ebiroll/qemu_esp32`): community
  QEMU fork for the original ESP32 advertising Bluetooth/WiFi support;
  not evaluated here, ESP32 (classic) only.
* **Vendor calibration documentation** — IDF "RF Calibration" guide (e.g.
  `https://docs.espressif.com/projects/esp-idf/en/release-v5.3/esp32/api-guides/RF_calibration.html`):
  documents partial/full/no RF calibration phases and per-board calibration
  data in NVS, but no register-level detail.

Unknown (not found in public sources):

* Any public model of the S3 REGI2C engine timing, SAR2/PWDET calibration
  sequence, BT EM window at `0x3FC00000`, or `0x60031078`/`0x60042000` RF
  registers.
* Public register names for `0x6000E050/0x5C/0x60` on ESP32-S3 (nearest
  relative: ESP32-C6 `LP_I2C_ANA_MST`, see §1.2).
* Wokwi's internal radio emulation technique (closed source; only their
  user-facing docs exist).
* Any confirmation that ESP32-S3 rev 0 vs rev 0.2 ROM differs for the analyzed
  code (esp-rom-elfs ships `esp32s3_rev0_rom.elf` only; all analyzed PCs match
  the observed traces exactly, so the analyzed ROM appears to be the same image
  family as the board's).

---

## 5. Register/bitfield table the QEMU RTC/analog models must cover per stage

Observed accesses only (from ROM/blob decodes above); "must" = required for the
observed ROM/blob code paths to progress; timing semantics marked where known.

### Stage W — Wi-Fi full RF calibration (`esp_wifi_start`)

| Address | Register / field (public name if any) | Observed required behavior |
| --- | --- | --- |
| `0x6000E000` | REGI2C engine-0 transaction word (unnamed; `(func<<26)\|BIT24\|(val<<16)\|(reg<<8)\|slave`) | accept writes; BIT25 = busy must self-clear |
| `0x6000E004` | REGI2C engine-0 second/parallel word (unnamed) | accept writes; used by `rom_i2c_paral_read` result path |
| `0x6000E040` | `I2C_MST_ANA_CONF0_REG` (`regi2c_defs.h`) | `I2C_MST_BBPLL_CAL_DONE` BIT24 behavior (current stub reads it as always set) |
| `0x6000E044` | `ANA_CONFIG_REG` | clear-bit-enables-slave semantics (`I2C_BBPLL_M` BIT17, `I2C_SAR_M` BIT18), written by `rom_i2c_paral_set_read` as `~mask` |
| `0x6000E048` | `ANA_CONFIG2_REG` | RMW by `rom_get_i2c_hostid` / `rom_i2c_paral_set_mst0` (`ANA_SAR_CFG2_M` BIT16 area) |
| `0x6000E050` | REGI2C/SAR-PWDET status (unnamed) | `[26:24]` must progress to `7` only after the SAR2/PWDET sequence; bit1 toggled by `rom_pkdet_vol_start` after the poll |
| `0x6000E05C` | REGI2C engine transaction/config (unnamed) | `[15:0] ← 0x016a`; BIT23 and BIT19 are operation/start bits set by `rom_pkdet_vol_start` |
| `0x6000E060` | REGI2C master config (unnamed) | base `0x00ff5012`-style value (from `rom_i2cmst_reg_init`); `pwdet_sar2_init` ORs `0x18`, clears bit 5 |
| `0x60008830` | `SENS_SAR_MEAS2_CTRL2_REG` | `SENS_SAR2_EN_PAD_FORCE` BIT31 set; bits [30:15] cleared |
| `0x60008834` | `SENS_SAR_MEAS2_MUX_REG` | `SENS_SAR2_RTC_FORCE` BIT31 cleared |
| `0x60040000+` | APB_SARADC (`reg_base.h`) | SAR2 measurement data path feeding `rom_read_sar_dout` (timed; result averaged 4×) |
| — | SAR2/PWDET conversion clock + reset ownership | conversion must take finite time and depend on the clock/reset state; never a constant DONE |

### Stage B1 — BLE controller LP-clock (`btdm_low_power_mode_init`, bt.c:1749-1769)

| Address | Register / field | Observed required behavior |
| --- | --- | --- |
| `0x600C0028` | `SYSTEM_BT_LPCK_DIV_INT_REG` / `SYSTEM_BT_LPCK_DIV_NUM [11:0]` | store + readback (verified write); default 255 |
| `0x600C002C` | `SYSTEM_BT_LPCK_DIV_FRAC_REG` / `SYSTEM_BT_LPCK_DIV_A [23:12]` (+ `LPCLK_SEL_*` [24:28], `LPCLK_RTC_EN` [28]) | store + readback of `[23:12]` (write-then-verify by `btdm_lpclk_select_src`); documented LPCLK source bits [24:28] exist in header |
| slow-clock path | `rtc_clk_slow_src_get()` sources (`SOC_RTC_SLOW_CLK_SRC_RC_SLOW`/`XTAL32K`), `esp_clk_slowclk_cal_get()` calibration (`bt.c:1719-1769`) | observable slow-clock source + calibration value for the non-main-XTAL branches |

### Stage B2 — BLE RF init (`r_rw_rf_init` / `r_rwip_init` path)

| Address | Register / field | Observed required behavior |
| --- | --- | --- |
| `0x3FC00000+0x100` | undocumented BT EM/modem RAM `[INFERENCE]` | mapped RAM window accepting the 40-byte RF-EM table copy (fault address `0x3fc00100`, size exercised ≥ `0x28`) |
| `0x60031078` | undocumented RF register window | `memw` RMW: keep `[31:16]`, set `[15:0]=0x0100`; value must persist |
| `0x60042000` | undocumented RF register window (`r_rf_sleep`) | `memw` RMW of bits `0x47`/`0x10` |
| `0x3fceff5c` area | `r_plf_funcs_p` platform table (`.bss`, runtime init) | entry at offset 188 called with arg `0x100`, returns EM destination pointer |

---

## 6. Explicit UNKNOWN list

1. Official name and semantics of `0x6000e050[26:24]` (3 status bits; only the
   "poll until 7" exit condition is observed; no public S3/C3/C6 field maps this
   offset).
2. Full field maps of `0x6000e05c` and `0x6000e060` (only the observed
   encodings `0x016a`/`0x8000016a`-style words and the `|0x18`/`&~0x20` RMW are
   pinned; slave byte `0x6a` vs `I2C_SAR_ADC 0x69` in `regi2c_saradc.h` is not
   reconciled).
3. Complete REGI2C engine layout for S3 (`0x6000E000-0x6000E0FF`): number of
   parallel engines, per-engine register strides, DATE register.
4. SAR2/PWDET conversion timing, ADC clock source/divider, and reset ownership
   during calibration (what legitimately delays status→7).
5. The 4-entry LPCLK select table values the blob writes into
   `SYSTEM_BT_LPCK_DIV_A [23:12]` per `ESP_BT_SLEEP_CLOCK_*` selection.
6. Size and silicon decode of the `0x3FC00000` window (only offsets
   `0x100..0x128` exercised; not present in any public map).
7. The ROM/platform implementation behind `r_plf_funcs_p` entry 188 (table is
   runtime-initialized; static ROM image shows only `.bss`).
8. Wi-Fi-native post-calibration behavior (scan/assoc etc.) — nothing past
   `esp_wifi_start` has been reached in QEMU; hardware capture proves those
   stages run on silicon but no register trace of them exists here.
9. Whether any vendor/community source documents the S3 REGI2C block internally
   (Espressif internal docs are not public).

## 7. Hardware vs QEMU summary (2026-10-07 captures)

| Point | Real silicon (rev 0.2) | Official QEMU 9.2.2 | Recovered core |
| --- | --- | --- | --- |
| Wi-Fi `esp_wifi_start` (full RF cal) | passes (`records.json`, `last_stage: esp_wifi_deinit`) | stalls at `0x40036a46` polling `0x6000e050[26:24]` | same stall |
| BLE LP-clock select/div (`bt.c:1757`) | passes | assert fires (readback fails) | passes (`esp32s3_clk.c` stores `0x600C0028/2C`) |
| BLE RF init EM access | passes | not reached (assert earlier) | `LoadStorePIFAddrError` at `0x3fc00100` inside ROM `memcpy` |

First divergence per lane: Wi-Fi → REGI2C/SAR-PWDET status machine (§1);
BLE-official → SYSTEM LPCK register readback (§2); BLE-recovered → `0x3FC00000`
window mapping (§3).
