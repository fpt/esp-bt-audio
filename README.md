# esp-bt-audio

Battery-powered Bluetooth headphone receiver. An iPhone streams over A2DP to an
ESP32-WROOM-32E, which drives a PT8211 I2S DAC and an LM4881 headphone amp.
One LiPo cell, charged over USB by a TP4056 module.

- Bluetooth Classic A2DP sink (SBC) + AVRCP (play/pause, phone absolute volume)
- Push-button soft power latch, battery monitor, automatic power-off
- WS2812B-V6 status LED
- Flashed through a 6-pin zig-zag header with an M5Stack ESP32-Downloader

## Boards

Two boards, connected by a 12-pin header (3V3, GND, BCLK, WS, DIN, MUTE, 3V3_EN, VDD).

| Directory | Board | Contents |
|---|---|---|
| `main/` | main | ESP32-WROOM-32E, power and BT buttons, LED, power latch (D1/D3), battery divider, programming header |
| `i2s/` | i2s-power | TP4056 module, RT9080 3.3 V LDO, PT8211, LM4881, 3.5 mm jack |

KiCad 10. Custom symbols and footprints (`MyLibrary:*`) come from the global library at
`../KiCad/MyLibrary.kicad_sym` / `../KiCad/MyLibrary.pretty`, not from this repo.

## Firmware

ESP-IDF v5.4 in Docker, see [firmware/README.md](firmware/README.md).

```bash
cd firmware
make build
make flash PORT=COM5
make monitor PORT=COM5
```

## Docs (Japanese)

- [docs/esp_bt_audio_receiver.md](docs/esp_bt_audio_receiver.md): whole-system design (power, pins, audio, controls, current budget, parts list)
- [docs/i2s_audio_design.md](docs/i2s_audio_design.md): PT8211 + LM4881 audio stage

## Layout

```
main/        main board (KiCad)
i2s/         i2s-power board (KiCad)
firmware/    ESP-IDF firmware
docs/        design notes
.claude/     Claude Code skills (vendored from aklofas/kicad-happy)
```

## Status

Boards designed, not yet fabricated. Firmware builds, not yet run on hardware.
