# Greenhouse CO₂ Monitor & Controller (ESP32)

A low-power IoT system that monitors and regulates CO₂ inside closed greenhouses, keeping it in the 800–1000 ppm range that is commonly targeted for crop growth, with Telegram and SMS alerts.


## Problem

In sealed greenhouses, CO₂ can drop well below outdoor levels during the day as plants photosynthesize, which limits growth. Too much CO₂ is also a problem for crops and for people working inside. A ventilation fan alone can only bring CO₂ down toward outdoor levels (~420 ppm), so enrichment needs a CO₂ source.

## Features

- CO₂ measurement with a Sensirion SCD41 (single-shot mode, low power)
- Two-way control: exhaust fan lowers CO₂, solenoid valve on a CO₂ cylinder raises it
- Day/night logic with a light sensor (enrichment only during daylight)
- Hysteresis, maximum valve-open time, and rest periods to avoid overshoot
- Deep sleep between checks (default: every 30 min); stays awake and re-checks every 30 s when CO₂ is out of range
- Telegram alerts over Wi-Fi, with automatic SMS fallback via a 4G modem
- Critical alerts (sensor failure, CO₂ too high, unstable control) are sent on both channels
- Fail-safe: valve and fan are switched off before sleep, and pin states are held during deep sleep

## Hardware

| Part | Purpose |
|---|---|
| ESP32 dev board | Controller |
| Sensirion SCD41 | CO₂ (plus temperature/humidity) |
| BH1750 | Light level (day/night) |
| 2-channel relay module | Fan and CO₂ valve |
| Solenoid valve + regulator + flow restrictor | CO₂ injection |
| Exhaust fan | Ventilation |
| A7670E 4G modem + SIM | SMS fallback |
| Separate 5 V / 2 A supply for the modem | Modem peak current |

## Wiring

| Signal | ESP32 pin |
|---|---|
| SCD41 + BH1750 SDA | GPIO 21 |
| SCD41 + BH1750 SCL | GPIO 22 |
| Fan relay | GPIO 25 |
| CO₂ valve relay | GPIO 27 |
| Modem TX → ESP32 RX1 | GPIO 32 |
| Modem RX ← ESP32 TX1 | GPIO 33 |

All grounds must be common. Switch mains-voltage loads only through properly rated relays and enclosures.

## How it works

1. The ESP32 wakes from deep sleep and measures CO₂ and light.
2. **Fan:** on above 1000 ppm by day (1500 ppm at night), off 100 ppm below the threshold.
3. **Valve:** opens only in daylight when CO₂ is below 800 ppm, closes at 850 ppm, and is limited to 2 minutes per pulse with a 5-minute rest.
4. The fan and valve are never on at the same time.
5. Once readings are stable, the device sends an hourly report and goes back to sleep.
6. If CO₂ exceeds 2000 ppm, the sensor stops responding, or control does not stabilize within 30 minutes, an alert is sent.

## Setup

1. Install the Arduino ESP32 core.
2. Open the sketch and edit the settings at the top: Wi-Fi, Telegram bot token and chat ID, and phone number.
3. If your relay module is active-LOW, swap `RELAY_ON` and `RELAY_OFF`.
4. Upload, then open the Serial Monitor at 115200 baud.

Create a Telegram bot with @BotFather and get your chat ID before first run.

**Do not commit credentials.** Keep them in a separate `secrets.h` listed in `.gitignore`.

## Testing tip

Test first without the fan, valve, or cylinder connected: check that the relays switch correctly and that alerts arrive. Set `SLEEP_MIN` to 1 for faster testing.

## Safety

CO₂ above ~5000 ppm is hazardous to people. Use a regulator with a flow restrictor, never enter the greenhouse during injection, and add an independent safety mechanism (for example, a mechanical limit or a second sensor) before any real deployment.

## Roadmap

- Google Sheets data logging
- Temperature/humidity-aware ventilation
- Web dashboard
- Field validation and calibration data
