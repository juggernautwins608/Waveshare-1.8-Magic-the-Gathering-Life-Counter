# Waveshare 1.8" Magic: The Gathering Life Counter

A polished **Magic: The Gathering** life & counter tracker for the **Waveshare
ESP32‑S3‑Knob‑Touch‑LCD‑1.8** — the round 360×360 rotary‑knob display. Spin the knob to change
your life total, tap the screen to select things, and track everything a game of Commander or
Standard throws at you, all on a battery‑powered puck that fits in your deck box.

> Each device tracks **one player's** own board state (life, the damage dealt *to* them by each
> opponent, their own counters) — so everyone at the table runs their own puck.

---

## Features

- **Formats:** Standard (starts at 20) and Commander (starts at 40) — switchable.
- **Life total:** huge, easy‑to‑read digits that **gradient green → yellow → red** as you drop,
  breathe faster near death, and flash on 0.
- **Opponents (Commander):** three corner dots; **tap a dot** to expand and track that opponent's
  **Commander damage** to you (x/21). "Move Opponents" rotates the seats to match your table.
- **Counters:** Poison, Energy, Storm — enable the ones you use; tap to expand, knob to edit.
- **Commander tax:** quick +2 tracker (CTAX).
- **First‑player spinner:** tap to spin an arrow that lands on a random seat.
- **Themes:** Midnight, Daylight, Forest, Crimson, Ocean.
- **Screen rotation:** 0 / 90 / 180 / 270° so the USB port can exit whichever side you like.
- **Battery‑friendly:** brightness (Dim/Low/Med/High), **auto‑sleep** (1/3/5 min or Off) with a
  pre‑dim step; the screen wakes on any knob turn or touch.
- **Battery readout:** live **% + voltage**, a **charge‑time estimate**, a self‑calibrating
  **"time left" runtime estimate**, a **charging ⚡** indicator, and a per‑device **battery‑capacity
  selector** (set it to match your LiPo).

## Controls

| Action | Does |
|---|---|
| **Turn knob** | Change the selected value (life by default) |
| **Tap an element** | Select / expand it (life, an opponent dot, a counter, tax) |
| **Tap the ⚙ gear** | Open Settings |
| **Tap buttons** | Navigate (Back / Back to game / Done, etc.) |

There is **no knob push‑button** on this board, so all selection is by tap.

---

## Hardware

- **Board:** [Waveshare ESP32‑S3‑Knob‑Touch‑LCD‑1.8](https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8)
  (panel **JC3636W518 V2**, ST77916 QSPI 360×360, CST816S touch, rotary knob).
- **Battery:** a 3.7 V single‑cell LiPo. These units commonly ship with a **102035 (~700 mAh)**
  cell (the firmware defaults to 700 mAh; change it in **Settings → Battery** for any other size).
- A **real USB‑C data cable** for flashing (see the flashing notes — this matters!).

Pin map (for reference): LCD CS14 / CLK13 / D0–D3 15‑18 / RST21 / Backlight47 · Touch SDA11 /
SCL12 / INT9 / RST10 (I²C 0x15) · Encoder A8 / B7 · Battery ADC = GPIO1 (2:1 divider).

---

## Build & install

### 1. Toolchain
- **Arduino IDE** (or arduino‑cli) with the **ESP32 Arduino core, version `2.0.14`**
  (Boards Manager URL: `https://espressif.github.io/arduino-esp32/package_esp32_index.json`).
  Newer 3.x cores change the ST77916 init and I²S APIs and are **not** supported here.
- **LVGL `8.3.6`** — install via Library Manager, then **replace its `lv_conf.h`** with the one in
  this repo's [`libraries/lv_conf.h`](libraries/lv_conf.h) (put `lv_conf.h` in your Arduino
  `libraries/` folder, next to the `lvgl` folder).
- **Arduino_GFX — use the PATCHED copy in this repo** at
  [`libraries/GFX_Library_for_Arduino`](libraries/GFX_Library_for_Arduino). Copy it into your
  Arduino `libraries/` folder. **Do not** use the stock Arduino_GFX from Library Manager — this
  panel needs the **ST77916 "V2" init table** or the screen shows striped garbage.

Your Arduino `libraries/` folder should end up containing: `GFX_Library_for_Arduino/` (this repo's
patched copy), `lvgl/` (8.3.6), and `lv_conf.h` (this repo's copy).

### 2. Board settings (Tools menu / FQBN)
`ESP32S3 Dev Module` with: **Flash Size 16MB**, **Partition Scheme "Huge APP (3MB No OTA/1MB SPIFFS)"**,
**PSRAM "OPI PSRAM"**, **USB CDC On Boot "Enabled"**.

FQBN: `esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=cdc`

### 3. Open & upload
Open [`MTG_Life_Counter/MTG_Life_Counter.ino`](MTG_Life_Counter/MTG_Life_Counter.ino), pick the
serial port, and Upload.

<details>
<summary>Headless build/flash with arduino-cli</summary>

```bash
# with lvgl 8.3.6, this repo's lv_conf.h, and this repo's patched GFX all in <libs>/
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=cdc \
  --libraries <libs> MTG_Life_Counter
arduino-cli upload -p /dev/cu.usbmodemXXXX \
  --fqbn esp32:esp32:esp32s3:FlashSize=16M,PartitionScheme=huge_app,PSRAM=opi,CDCOnBoot=cdc \
  MTG_Life_Counter
```
</details>

---

## ⚠️ Flashing notes (read this — it will save you hours)

This board has quirks that make flashing fail in confusing ways:

1. **Use a real USB‑C *data* cable.** Charge‑only cables enumerate but don't carry the reset
   control lines, so uploads fail with `Failed to connect to ESP32-S3: No serial data received`.
   No amount of retrying fixes it — swap to a known‑good data cable.
2. **The USB‑C plug orientation selects which chip you talk to.** This is a dual‑MCU board sharing
   one USB‑C port. One orientation reaches a secondary ESP32 (esptool reports `ESP32-U4WDH`,
   port `/dev/cu.usbserial*`); **flip the plug 180°** to reach the main **ESP32‑S3** (esptool
   reports `ESP32-S3`, port `/dev/cu.usbmodem*`) — that's the one you flash.
3. **Close any serial monitor** before uploading (it can hold the port).
4. There's **no external BOOT button** — a normal upload auto‑resets the board over a proper data
   cable, so #1 and #2 are what matter.

Quick check you're on the right chip:
```bash
esptool.py --chip auto -p /dev/cu.usbmodemXXXX chip_id   # must say "Chip is ESP32-S3"
```

---

## Battery behavior (and its honest limits)

Battery voltage is read on GPIO1 through an on‑board 2:1 divider. There is **no charge‑status pin**,
so charge state is inferred from voltage:

- **While charging**, the USB rail pins the voltage high (~4.7 V), so a true % can't be read. The
  display shows your **last real on‑battery level** with a **⚡** rather than a fake "100%".
- **To read the true %:** unplug, wait ~10 s, and check **Settings → Battery** — resting
  **~4.15–4.20 V ≈ full**.
- **Charge‑time / runtime estimates** are approximate (voltage→% is coarse; ±~20%). The runtime
  "time left" self‑calibrates from actual drain and accounts for brightness/sleep.
- **Set your battery size** in Settings → Battery so the charge‑time math is right for your cell.

For fastest charging, charge with the screen asleep or the unit switched off (a running screen
draws current that competes with charging).

---

## Repo layout

```
MTG_Life_Counter/       Arduino sketch (open the .ino here)
  MTG_Life_Counter.ino
  game_engine.h         board-independent game logic
  life_font_88.c        88px LVGL font for the life total
libraries/
  GFX_Library_for_Arduino/   PATCHED Arduino_GFX (ST77916 V2 init) — required
  lv_conf.h                  LVGL config — drop in next to your lvgl install
```

---

## Credits & licenses

- Application code: **MIT** (see [LICENSE](LICENSE)).
- **Arduino_GFX** ("GFX Library for Arduino"): BSD 3‑Clause, © Adafruit Industries &
  moononournation. The bundled copy is **patched** — the ST77916 init table is replaced with the
  JC3636W518 **V2** sequence (adapted from Waveshare's board demo). License retained in
  `libraries/GFX_Library_for_Arduino/license.txt`.
- **LVGL** 8.3.6: MIT (installed separately).

## Disclaimer

Hobby project, provided **as‑is**. Lithium batteries can be dangerous — use a proper protected
LiPo, don't charge unattended the first time, and stop using any cell that swells or gets hot.
