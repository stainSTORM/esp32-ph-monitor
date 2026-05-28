# ESP32 pH & Temperature Monitor

A firmware for the ESP32-S3 that monitors liquid pH and temperature and integrates with [Arkitekt](https://arkitekt.live) for real-time data streaming, remote control, and guided pH calibration — all from the Arkitekt Orchestrator UI.

![Hardware setup](docs/image.png)

## What It Measures

- **pH** — A standard BNC pH probe submerged in the liquid outputs a voltage. The firmware converts this to a pH reading using a temperature-compensated, multi-point calibration curve.
- **Liquid temperature** — A DS18B20 probe submerged alongside the pH probe compensates the pH reading for temperature drift (Nernst equation) and reports the liquid temperature directly.
- **Ambient temperature & pressure** — A BMP280 breakout board monitors the environment around the device (not the liquid).

Both the pH probe and the DS18B20 must be submerged in the liquid being measured. The BMP280 sits outside.

## Hardware

### Components

| Component | Role |
|-----------|------|
| ESP32-S3 (XH-S3E, 16 MB flash) | Main controller |
| Analog pH probe + BNC module | Measures liquid pH via ADC |
| DS18B20 waterproof temperature probe | Measures liquid temperature (also used for pH temperature compensation) |
| BMP280 breakout (I2C, 3.3 V) | Ambient temperature + atmospheric pressure |

### Wiring

| Signal | ESP32-S3 pin |
|--------|-------------|
| pH probe signal | GPIO 1 (ADC) |
| DS18B20 data | GPIO 4 |
| BMP280 SDA | GPIO 8 |
| BMP280 SCL | GPIO 9 |

Connect the BMP280 breakout's VIN to 3.3 V and GND to GND.

## Features

### Continuous sensor readings

Every 5 seconds the firmware:

1. Takes 16 ADC samples of the pH probe and averages them (reduces noise)
2. Reads the DS18B20 liquid temperature via OneWire
3. Applies temperature-compensated pH conversion using the isopotential-point approximation
4. Reads BMP280 ambient temperature and pressure
5. Pushes all values to Arkitekt as live state

### pH calibration

The firmware uses N-point least-squares linear regression over the pH–voltage calibration pairs you record. This is more robust than a fixed two-point approach:

- Up to 5 buffer solutions can be used
- The full calibration curve is refitted after each new point
- With 3 or more points, electrode linearity is checked automatically — if any recorded point deviates more than 0.3 pH units from the fitted line, a warning is issued suggesting probe replacement
- The nearest stored point is replaced when you recalibrate close to an existing pH value (within ±0.2 pH), so recalibrating a single buffer does not accumulate duplicate points
- Calibration is stored in NVS (non-volatile flash storage) and survives reboots

**Calibration workflow in Arkitekt Orchestrator:**

1. Pour a fresh buffer solution (e.g. pH 6.86) into a clean container
2. Submerge the pH probe and wait 1–2 minutes for the reading to stabilize
3. In the Orchestrator, call `calibrate_ph_point` with `buffer_ph = 6.86`
4. Rinse the probe with distilled water and repeat with a second buffer (e.g. pH 4.01)
5. Optionally add a third buffer (e.g. pH 9.18) — this enables the automatic linearity check
6. Call `get_calibration` to inspect the fitted regression line and all stored points

**Buffer solution reference:**

| Color | Typical pH |
|-------|-----------|
| Red | 4.01 |
| Yellow | 6.86 or 7.00 |
| Blue | 9.18 or 10.01 |

A healthy electrode slope is typically −0.05 to −0.07 V/pH. If the linearity warning triggers, the electrode response is non-linear and the probe may need replacing.

### Arkitekt integration

Once provisioned, the device registers as an Arkitekt agent and exposes the following from the Orchestrator UI:

**Functions**

| Function | Description |
|----------|-------------|
| `calibrate_ph_point` | Record a calibration point at a known buffer pH. Returns the measured voltage, updated regression parameters, and a linearity warning if the electrode deviates. |
| `get_calibration` | Return all stored calibration points and the current regression slope, intercept, and linearity residual. |
| `reset_calibration` | Clear all calibration points and return to theoretical defaults. |
| `read_sensors` | Trigger an immediate reading from all sensors (pH, liquid temp, ambient temp, pressure). |
| `toggle_led` | Toggle the built-in LED — useful to confirm the device is responsive. |

**Live states** (updated every 5 seconds)

| State | Ports |
|-------|-------|
| `ph_status` | pH, raw ADC voltage, liquid temperature, DS18B20 OK, calibration point count, calibration slope, calibration linearity OK, reading count |
| `environment_status` | ambient temperature, pressure, BMP280 OK, reading count |
| `led_status` | on/off, GPIO pin |

## Setup

### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- An [Arkitekt](https://arkitekt.live) server instance
- The [Pokket](https://github.com/jhnnsrs/pokket) app on Android for BLE provisioning

### Build & flash

```bash
git clone https://github.com/stainSTORM/esp32-ph-monitor.git
cd esp32-ph-monitor
pio run -e xh-s3e-ph-monitor --target upload
```

### Provision

After flashing, open **Pokket** on your Android phone. Pokket delivers Wi-Fi credentials and an Arkitekt connection token to the ESP32 over BLE — no hardcoded secrets needed. WPA2-Enterprise (Eduroam) is supported.

On boot the device waits 5 seconds for a BLE connection. If valid credentials are already stored in flash it reconnects automatically without requiring Pokket again.

## Dependencies

| Library | Purpose |
|---------|---------|
| [ArduinoJson](https://arduinojson.org/) | JSON for the Arkitekt agent protocol |
| [WebSockets](https://github.com/Links2004/arduinoWebSockets) | WebSocket client |
| [OneWire](https://github.com/PaulStoffregen/OneWire) + [DallasTemperature](https://github.com/milesburton/Arduino-Temperature-Control-Library) | DS18B20 probe |
| [Adafruit BMP280](https://github.com/adafruit/Adafruit_BMP280_Library) | Ambient temperature + pressure |
| ESP32 BLE (built-in) | BLE provisioning |
| Preferences (built-in) | NVS calibration persistence |

## License

MIT
