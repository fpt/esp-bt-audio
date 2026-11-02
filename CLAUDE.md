# esp-bt-audio — Claude context

Bluetooth headphone receiver: ESP32-WROOM-32E (A2DP sink) → PT8211 → LM4881, one LiPo
via a TP4056 module. Two KiCad boards (`main/`, `i2s/`), ESP-IDF firmware (`firmware/`).
See README.md for layout and `docs/esp_bt_audio_receiver.md` for the design.

## Rules

- Docs under `docs/` are in Japanese. Keep them in sync with the schematics and the
  firmware (pins, part values, behaviour) when either changes.
- Firmware builds only through `make build` in `firmware/` (Docker, `espressif/idf:v5.4.1`).
  Host Python only through `uv run`. `sdkconfig.defaults` holds deltas; `make build`
  regenerates `sdkconfig` when it or `main/Kconfig.projbuild` is newer.
- Pins in `firmware/main/Kconfig.projbuild` must match `main/esp-bt-audio.kicad_sch`.
- Don't edit `.kicad_sch` / `.kicad_pcb` by hand unless asked; the user edits in KiCad.
- The root `.gitignore` drops `*.csv` (KiCad exports); `firmware/.gitignore` re-includes
  `partitions.csv`.

## Checking the boards

`kicad-cli` is at `C:\Program Files\KiCad\10.0\bin\kicad-cli.exe` (not on PATH).

```sh
K="/c/Program Files/KiCad/10.0/bin/kicad-cli.exe"
"$K" sch erc --severity-all -o erc.rpt main/esp-bt-audio.kicad_sch
"$K" pcb drc --refill-zones --schematic-parity --severity-all -o drc.rpt main/esp-bt-audio.kicad_pcb
"$K" sch export netlist -o net.net main/esp-bt-audio.kicad_sch
```

- `--schematic-parity` needs the `.kicad_sch` next to the board; run it in place, not on
  a copy. `--refill-zones` does not save the board.
- Write reports and netlists to the scratchpad, not into the repo.
- `MyLibrary:*` symbols/footprints live in the global library `../KiCad/MyLibrary.*`.
  Pin numbering of a symbol must match its footprint (the WS2812C-2020 symbol on a
  5050 footprint was wrong once).
- `.claude/skills/kicad` (kicad-happy) has deeper analyzers; ask before running them,
  they are external code.

## Hardware notes

- TP4056 module (`GENERIC_TP4056_Module_wo_bat`): IN− and OUT− are the same node on the
  module and both go to GND. BAT_N must not go to GND (that bypasses the protection FETs).
- Power latch: SW5 → D3 → 3V3_EN (RT9080 EN, 100k pull-down on i2s-power); GPIO15 → D1
  holds it. GPIO34 reads SW5 through 22k/100k. Firmware drives GPIO15 high first thing
  in `app_main`, and power actually drops only when SW5 is released.
- All loads except the battery divider run from the RT9080 3.3 V output, so the board
  draws ~µA when off. WS2812B-V6 is rated 3.3–5.3 V.
- LM4881 SHUTDOWN is active high, pulled up to 3.3 V on i2s-power (amp off by default).
- ESP32 EN has 10k + 1µF on the main board; the module has no internal pull-up.
- Strapping: keep GPIO12 low/unconnected. GPIO15 is used for the power latch on purpose.

## Firmware notes

- Adapted from `../rp-esp-wifi-bta` (`components/at_btaudio`, ESP-AT based) without the
  AT layer, Wi-Fi switching and metadata.
- Bluedroid callbacks run in the BTC task; the A2DP data callback (`audio_feed`) must not
  block. I2S rate changes are applied in `audio_task`, not in the callback.
- PT8211 is LSB-justified 16 bit; with 16-bit slots that equals MSB mode
  (`I2S_STD_MSB_SLOT_DEFAULT_CONFIG`).
- Flash via the Downloader with the board off (the Downloader's 3V3 feeds the rail).

## Not verified on hardware yet

Everything: boards are not fabricated. Open questions: PT8211 L/R order
(`CONFIG_BTR_SWAP_LR`), WS2812B-V6 colour order, battery ADC accuracy, LM4881 pop on
amp enable.
