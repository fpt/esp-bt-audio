# esp-bt-audio firmware

ESP-IDF v5.4 firmware for the main board (ESP32-WROOM-32E): Bluetooth A2DP sink + AVRCP,
decoded audio out over I2S to the PT8211 on the i2s-power board, LM4881 headphone amp.
The Bluetooth and LED code is adapted from `../../rp-esp-wifi-bta`
(`components/at_btaudio`), without its AT command layer.

Hardware: [../docs/esp_bt_audio_receiver.md](../docs/esp_bt_audio_receiver.md).

## Build and flash

Requires Docker and [uv](https://docs.astral.sh/uv/) on the host. Nothing else is installed.

```bash
make build                 # ESP-IDF in Docker (espressif/idf:v5.4.1), output in build/
make flash PORT=COM5       # via the M5Stack ESP32-Downloader on the J1 header
make monitor PORT=COM5     # log console (UART0), 115200
make erase PORT=COM5       # clears NVS (pairing, volume)
make menuconfig            # pins and options: "BT Audio Receiver"
make help
```

On macOS/Linux `PORT` is auto-detected. Turn the board off before flashing from the
Downloader's 3V3 (see the receiver doc, section 7).

## Pins

| GPIO | Function |
|---|---|
| 26 / 25 / 27 | I2S BCK / WS / DATA → PT8211 |
| 23 | LM4881 SHUTDOWN (high = off) |
| 15 | Power latch (high keeps the RT9080 on) |
| 34 | Power button sense (SW5, high = pressed) |
| 13 | BT button (SW6, low = pressed) |
| 14 | WS2812B status LED |
| 35 | Battery voltage (1/2 divider, ADC1) |

All pins can be changed in menuconfig (`main/Kconfig.projbuild`).

## Behaviour

| Control | Action |
|---|---|
| Power button, press | power on (hold until the LED lights) |
| Power button, hold 2 s | power off (LED red; power drops on release) |
| BT button, press | play / pause |
| BT button, hold 3 s | pairing mode (drops the current phone, discoverable) |

| LED | State |
|---|---|
| blue, fast blink | pairing (discoverable) or connecting |
| blue, short blip | waiting for the last phone |
| blue, steady | connected |
| green | playing |
| red blink overlay | battery low (< 3.4 V) |
| red, steady | powering off |

- After power-on the receiver is discoverable for 2 minutes and reconnects to the last phone.
- The phone's volume (AVRCP absolute volume) scales the samples before the PT8211.
- The amp turns on when audio starts and off 5 s after it stops.
- Automatic power-off: battery below 3.2 V, or 10 minutes without a connection.

## Layout

```
main/
  main.c        boot order: power latch first, then NVS, audio, power, UI, BT
  bt.c          GAP / A2DP sink / AVRCP CT+TG (from bta_bt.c)
  audio.c       ring buffer -> volume -> I2S (PT8211), amp control
  ui.c          buttons + WS2812B over RMT (from bta_ui.c)
  power.c       power latch, battery ADC, auto power-off
  Kconfig.projbuild
sdkconfig.defaults   deltas on IDF defaults (Classic BT only, 160 MHz, -O2)
partitions.csv       single 3 MB factory app
```

## Not verified on hardware yet

Everything. In particular: PT8211 left/right order (`CONFIG_BTR_SWAP_LR`), WS2812B-V6
colour order and timing, battery reading accuracy.
