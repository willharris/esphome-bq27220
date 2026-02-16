# CLAUDE.md — ESPHome BQ27220 for Arduino Nesso N1

## Project Overview

ESPHome external component for the TI BQ27220 fuel gauge IC, adapted for the Arduino Nesso N1 board. Originally forked from [webash/esphome-bq27220](https://github.com/webash/esphome-bq27220) (which was based on the LILYGO T-Embed / Flipper Zero BQ27220 driver).

The Nesso N1 has a complete power management subsystem that requires software initialization — the charger does **not** charge by default.

## Repository Structure

```
components/bq27220/
  __init__.py          # ESPHome component registration
  sensor.py            # ESPHome sensor platform (YAML schema)
  bq27220.h            # Component class, I2C helpers, register structs
  bq27220.cpp          # Component logic: setup, update, init, CEDV config
  bq27220_def.h        # Register addresses, command codes, keys
  bq27220_data_memory.h  # DMData struct, DM address enums, gauging config struct
  bq27220_data_memory.c  # CEDV parameters for 250mAh Nesso N1 battery
nesso-diagnostic.yaml  # Full ESPHome config for Nesso N1
```

## Hardware: Arduino Nesso N1

- **SoC**: ESP32-C6 (single-core RISC-V, WiFi 6, BLE 5, 802.15.4)
- **Battery**: 3.7V 250mAh LiPo (single cell)
- **Display**: ST7789V 135x240 TFT (SPI)
- **Touch**: FT6336U capacitive (I2C 0x38)
- **LoRa**: SX1262 (SPI)
- **IMU**: BMI270 (I2C 0x68)
- **GPIO Expanders**: 2× PI4IOE5V6408 (I2C 0x43, 0x44)
- **Docs**: https://docs.arduino.cc/hardware/nesso-n1

### I2C Bus (SDA=GPIO10, SCL=GPIO8, 50kHz)

| Address | Device | Notes |
|---------|--------|-------|
| 0x38 | FT6336U | Touchscreen controller |
| 0x43 | PI4IOE5V6408 | GPIO expander 0 (buttons, LoRa) |
| 0x44 | PI4IOE5V6408 | GPIO expander 1 (power/UI) |
| 0x49 | AW32001ECSR | LiPo charge controller |
| 0x55 | BQ27220 | Fuel gauge (absent when SHUTDOWN) |
| 0x68 | BMI270 | IMU |

### Power Path

```
USB-C → AW32001 (charger, 0x49) → 250mAh LiPo → JW5712 buck (3.3V) → system
```

## AW32001ECSR Charge Controller

**Critical**: The AW32001 powers up with **charging disabled** (CEB=1 in REG01). A host must clear the CEB bit via I2C to enable charging. The Arduino `NessoBattery` library does this; ESPHome has no built-in AW32001 driver, so the YAML `on_boot` lambda handles it.

### Key Registers

| Register | Bits | Description |
|----------|------|-------------|
| REG00 | [6:3] VIN_DPM, [2:0] IIN_LIM | Input voltage/current limits |
| REG01 | [3] CEB | **Charge Enable**: 0=enabled, 1=disabled (default) |
| REG02 | [6] WD_TMR_RST, [5:0] ICHG | Watchdog kick + charge current |
| REG08 | [4:3] CHG_STAT, [1] PG_STAT | Charge status, power good |
| REG09 | Various | Fault register (read-clear) |

### Watchdog Timer

Default 160s timeout. If not kicked, **all registers reset to defaults** (re-disabling charging). The YAML interval lambda kicks the watchdog every 30s by writing to REG02.

### CHG_STAT Values (REG08 bits [4:3])

- 0 = Not charging
- 1 = Pre-charge
- 2 = Fast charge (CC/CV)
- 3 = Charge done

### Datasheet

https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1157/AW32001E.pdf

## BQ27220 Fuel Gauge

TI CEDV (Compensated End-of-Discharge Voltage) fuel gauge. I2C address 0x55. Device ID 0x0220.

### SHUTDOWN Mode

The BQ27220 enters SHUTDOWN when battery voltage drops below ~2.8V. In this state it is **completely absent from the I2C bus** (NACKs all transactions). It wakes when battery voltage rises above threshold or GPOUT is toggled.

**Boot sequence implication**: If the battery was deeply discharged, the gauge won't respond at `setup()` time. The component handles this via auto-recovery in `update()` — when the gauge appears on the bus, it runs `init()` to apply CEDV configuration.

### CEDV Configuration

The `bq27220_data_memory.c` file contains the battery profile. Key parameters:

- **DesignCapacity / FullChargeCapacity**: 250 mAh (Nesso N1 battery)
- **EMF**: 3680 mV (open-circuit voltage at ~50% SOC)
- **R0, R1**: Internal resistance parameters (scaled up from T-Embed values for smaller cell)
- **T0, TC, C0, C1**: Temperature and capacity compensation coefficients
- **DOD0-DOD100**: Voltage vs depth-of-discharge profile (generic LiPo curve)
- **EDV0/1/2**: End-of-discharge voltage thresholds (3200/3250/3300 mV)
- **FCC_LIM=1**: Caps FCC to Design Capacity
- **FIXED_EDV0=1**: Uses fixed EDV0 threshold

The R0/R1 and DOD voltage profile are estimated. For production accuracy, generate values using TI's GAUGEPARCAL tool (https://www.ti.com/tool/GAUGEPARCAL) with the actual cell's discharge curves.

### init() Flow

1. Verify device ID (0x0220)
2. Unseal the gauge (UnsealKey1 + UnsealKey2)
3. Check operation status (INITCOMP, CFGUPDATE)
4. Compare current data memory against `gauge_data_memory[]`
5. If mismatch: reset gauge → get full access → write new config → verify
6. Re-seal the gauge

### Security Modes

- **Sealed** (default): Read-only access to standard commands
- **Unsealed**: Read access to data memory
- **Full Access**: Write access to data memory (required for config update)

### Standard Commands (register addresses)

| Register | Address | Description | Unit |
|----------|---------|-------------|------|
| Control | 0x00 | Control/status subcommands | — |
| Voltage | 0x02 | Battery voltage | mV |
| Current | 0x0A | Instantaneous current | mA (signed) |
| Temperature | 0x06 | Battery temperature | 0.1K |
| StateOfCharge | 0x1C | SOC | % |
| RemainingCapacity | 0x10 | Remaining capacity | mAh |
| FullChargeCapacity | 0x12 | Full charge capacity | mAh |
| DesignCapacity | 0x3C | Design capacity | mAh |
| StateOfHealth | 0x2E | State of health | % |

Temperature conversion: `°C = (raw * 0.1) - 273.15`

### Datasheet

https://www.ti.com/product/BQ27220

## Bugs Fixed from Upstream

### 1. ErrorCode Inversion (critical, caused OTA rollback crashes)

ESPHome's `I2CDevice::write_register()` and `read_register()` return `i2c::ErrorCode` where `ERROR_OK = 0`. The original code used `!this->write_register(...)` which **inverts the logic** — treating success (0) as failure.

**Fix**: All calls changed to `!= i2c::ERROR_OK`.

```cpp
// WRONG (original):
if(!this->write_register(...)) { /* error */ }

// CORRECT (fixed):
if(this->write_register(...) != i2c::ERROR_OK) { /* error */ }
```

This applies to `parameterCheck()` and `dateMemoryCheck()` in the update path.

### 2. init() Never Returns True

The original `init()` function initializes `result = false` but never sets it to `true` on success. The function always returned `false`.

**Fix**: Added `result = true;` before the final `return result;` after all steps succeed.

### 3. No SHUTDOWN Recovery

Original code assumed the gauge is always present. If absent at boot, sensors published garbage values (e.g., 55756 mV, 65428 mAh).

**Fix**: 
- `setup()` probes the gauge; if absent, sets `gauge_available_ = false`
- `update()` checks `gauge_available_` and publishes NaN (shows "unavailable" in HA) if gauge is absent
- Auto-recovery: each `update()` cycle re-probes, and when gauge responds, runs `init()` if needed
- `cedv_configured_` flag tracks whether CEDV config has been applied

### 4. Goto Crossing Variable Declarations

Original code used `goto` statements that crossed variable declarations — undefined behavior in C++.

**Fix**: Replaced with `do { ... } while(0)` / `break` pattern (already present in most of the codebase).

## ESPHome YAML Notes

### Boot Priority

Component `setup()` runs at priority 600 (DATA). The `on_boot` lambda also runs at priority 600 but **after** component setup. This means:

1. BQ27220 `setup()` runs → gauge may be in SHUTDOWN → marks unavailable
2. `on_boot` runs → enables AW32001 charger → battery starts charging
3. ~30s later → `update()` detects gauge → runs `init()` with CEDV config

This is why the deferred `init()` in `update()` is important.

### Charger Initialization (on_boot lambda)

The `on_boot` lambda at priority 600:
1. Reads AW32001 chip ID (REG0B, expect 0x49)
2. Reads REG01, clears CEB bit (bit 3) → enables charging
3. Kicks watchdog via REG02
4. Reads REG08 for charge status
5. Reads REG09 for faults
6. Probes BQ27220 at 0x55 (ACK check)

### Charger Watchdog (interval lambda, 30s)

Every 30 seconds:
1. Kicks AW32001 watchdog (REG02 = 0x4F)
2. Defensively re-enables charging if CEB was reset
3. Logs decoded status: VIN_DPM, IIN_LIM, CHG_STAT, PG_STAT, faults, CEB state
4. ACK-checks BQ27220 presence

### External Component Reference

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/willharris/esphome-bq27220
      ref: adapt-for-nesso-n1
    components: [bq27220]
    refresh: 120s
```

## Known Issues / TODO

### CRC Write Failures During Config Update

During `dateMemoryCheck(data_memory, true)`, the checksum writes to `CommandMACDataSum` (0x60) consistently NACK. Despite this, the subsequent verification read passes and the config takes effect. The write protocol for the checksum register may need investigation — possibly needs only 2 bytes (checksum + length) rather than `size + 2` bytes.

### Temperature Reading

Battery temperature reads ~49°C during fast charge at 128mA. This is warm but within spec for a small LiPo cell. Should decrease as charge completes.

### CEDV Parameter Accuracy

The R0, R1, and DOD voltage profile values in `bq27220_data_memory.c` are estimates scaled from a 1300mAh cell. For accurate SOC tracking across the full discharge range, these should be calibrated using TI GAUGEPARCAL with the actual cell's discharge curves. The current values are sufficient for basic operation — the gauge's learning algorithm will self-correct FCC after charge/discharge cycles.

### No Battery-Only Runtime Test Yet

Device has been verified charging and reporting correct values on USB power. Battery-only operation (USB disconnected) needs testing to confirm:
- Device stays alive on battery
- Gauge tracks discharge correctly  
- Low-battery shutdown works properly
