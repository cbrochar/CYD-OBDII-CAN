# CYD-OBDII-CAN — Diesel Diagnostics Dash (direct CAN)

A touch-screen gauge display for a **2005 Ford Excursion 6.0L Powerstroke**, built on the
ESP32 "Cheap Yellow Display" (CYD). It is wired **straight to the truck's CAN bus** through the
ESP32's built-in CAN controller (TWAI) and a small 3.3 V transceiver, and shows live engine,
transmission and ABS data on swipeable pages of gauges, plus a list of stored trouble codes.

This is the CAN version of [CYD-OBDII](https://github.com/cbrochar/CYD-OBDII), which uses a
Bluetooth ELM327 adapter instead. The screens, gauges and PIDs are the same.

When the truck isn't answering it runs on demo values, so you can test the screens on the bench.

| Page 1: gauges | Page 3: ABS |
|---|---|
| ![Page 1 with transfer case, turbo boost, speed and trans temp](docs/images/page1-gauges.jpg) | ![ABS page with wheel speeds and ABS warning card](docs/images/page3-abs.jpg) |
| **Full-screen gauge** | **Page 4: stored error codes** |
| ![Turbo boost shown full screen](docs/images/fullscreen-turbo-boost.jpg) | ![Stored error codes page listing P0470 and P2291](docs/images/page4-error-codes.jpg) |

*The photos show demo mode (adapter not connected), so all the values are sample data.*

> **Help wanted:** some values still need the right Ford PIDs. See the
> [open issues](../../issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22) if you know your way
> around 6.0L / Super Duty modules.

---

## Features

- **5 pages**, tap to move between them (page dots at the bottom show where you are)
  1. Transfer case · Turbo boost · Vehicle speed · Transmission temp
  2. FICM main power · Coolant temp · System volts · Engine RPM
  3. ABS: Left-front wheel speed · Right-front wheel speed · Rear axle speed · ABS status (warning triangle when the ABS module has stored codes)
  4. Stored engine trouble codes (up to 15, e.g. `P0470`), red when codes are present, green when clear
  5. **CAN sniffer** (listen-only) with **SD card recording**, see [Listen-only sniffer](#listen-only-sniffer)
- **Tap menu**: tap any gauge to get **NEXT PAGE** and **FULL SCREEN** buttons.
  Full screen shows that one gauge as large as it fits; tap again to go back.
- **Status bar** that never scrolls: connection state, demo mode, retry countdown, and a
  red warning when engine trouble codes are stored.
- **Demo mode**: runs on sample values whenever the PCM isn't answering (key off, not plugged
  in), and probes it again every 5 seconds.
- **Fast polling**: only the values on screen are read from the truck. Talking to the bus
  directly takes a few milliseconds per value instead of about 100 ms through an ELM327.

## Hardware

| Part | Notes |
|---|---|
| ESP32-2432S028R ("Cheap Yellow Display") | 2.8" 320×240 ILI9341 screen, XPT2046 resistive touch |
| microSD card (optional) | For recording CAN traffic. FAT32; any size up to 32 GB is simplest |
| SN65HVD230 CAN transceiver board | 3.3 V CAN transceiver (the common blue "VP230" breakout). Do **not** use a 5 V TJA1050/MCP2551 board without level shifting. |
| OBD-II plug or pigtail | Male J1962 connector to reach CAN-H / CAN-L |
| 3D-printed case | [Modular CYD case](https://www.thingiverse.com/thing:6991968) by DanielBrunod, remixed from [ncmreynolds' CYD enclosure](https://www.thingiverse.com/thing:6308391) (CC BY-SA). STL files in [`hardware/case/`](hardware/case/) |
| USB power in the truck | 5 V via the CYD's USB port |

## Wiring

The transceiver connects to the CYD's **CN1** connector (the 4-pin JST: GND, IO22, IO27, 3.3V),
so no soldering on the CYD is needed. The TX/RX pins are set in [`src/config.h`](src/config.h)
(`CAN_TX_PIN`, `CAN_RX_PIN`). RX has to stay on IO27 (or another RTC-capable pin) so that bus
traffic can wake the board from deep sleep; IO22 can't do that.

| CYD CN1 | SN65HVD230 board |
|---|---|
| 3.3V | 3V3 |
| GND | GND |
| IO22 | CTX (D) |
| IO27 | CRX (R) |

| SN65HVD230 board | OBD-II port |
|---|---|
| CANH | pin 6 (CAN-H) |
| CANL | pin 14 (CAN-L) |
| GND | pin 5 (signal ground) |

- The truck's bus is already terminated (about 60 Ω between pins 6 and 14 with the key off).
  Most SN65HVD230 boards have a 120 Ω resistor across CANH/CANL: **remove it** (or cut its
  jumper), since a third terminator loads the truck's bus.
- Keep the CANH/CANL wires twisted together and short.
- Power the CYD from 5 V USB as before. If you power it from OBD pin 16 (battery, always on),
  use a 12 V → 5 V buck converter and read the battery-drain to-do in the [Roadmap](#roadmap).

## Vehicle / bus facts

These were confirmed on the truck with a PC and [`tools/obd_logger.py`](tools/) (see
[PC logging tool](#pc-logging-tool)):

- Protocol: **ISO 15765-4 CAN, 11-bit, 500 kbps** (ELM protocol 6, `ATSP6`), *not* J1850 PWM
- Engine PCM: requests to **7E0**, replies from **7E8**. A second module (7E9, likely the
  transmission) also answers standard requests.
- Standard PIDs confirmed working: `0104 0105 010B 010C 010D 010F 0110 011F 012F 0133 0142`
- Ford-specific FICM PID `2209D0` answers; the raw value is volts × 256
  (e.g. `0x2F80` → 47.5 V)

## Where each value comes from

| Gauge | Module / header | Request | Formula | Status |
|---|---|---|---|---|
| Turbo boost | PCM `7E0` | `010B` MAP, `0133` baro | (MAP − baro) kPa × 0.145 → psi, clamped at 0 | ✅ Confirmed |
| Vehicle speed | PCM `7E0` | `010D` | A km/h × 0.621 → mph | ✅ Confirmed |
| Engine RPM | PCM `7E0` | `010C` | (A×256 + B) / 4 | ✅ Confirmed |
| System volts | PCM `7E0` | `0142` | (A×256 + B) / 1000 | ✅ Confirmed |
| FICM main power | PCM `7E0` | `2209D0` | raw / 256 V | ✅ Confirmed |
| Coolant temp | PCM `7E0` | `221139` | raw / 256 °C → °F | ⚠️ Not yet confirmed |
| Trans temp | PCM `7E0` | `221E1C` | raw / 8 °F | ⚠️ Not yet confirmed |
| Engine trouble codes | PCM `7E0` | `03` | CAN Mode 03, multi-frame aware | ⚠️ Not yet confirmed |
| Transfer case | `418F02` | `2211B2` | 1 = 2H, 2 = 4H, 4 = 4L | ❌ Returns NO DATA; real module address needed |
| Wheel speeds (LF/RF/rear) | ABS `760` | placeholders | placeholder scale | ❌ Placeholders; get real PIDs from FORScan |
| ABS trouble codes | ABS `760` | `1800FF00` | KWP service 18 count | ⚠️ Not yet confirmed |

Turbo boost uses standard PID `0B`, which tops out at 255 kPa, so the gauge maxes out at about 22 psi of boost.

## Getting started

### 1. Tools
- [VS Code](https://code.visualstudio.com/) with the **PlatformIO** (or pioarduino) extension
- Libraries are installed automatically from `platformio.ini`:
  `TFT_eSPI 2.5.43`. CAN uses the ESP32's built-in TWAI driver, the SD card the built-in `SD`
  library, and touch has its own small driver ([`src/touch_bitbang.h`](src/touch_bitbang.h))

### 2. Wire the transceiver
See [Wiring](#wiring). If you used other GPIO pins, change `CAN_TX_PIN` / `CAN_RX_PIN` in
[`src/config.h`](src/config.h).

### 3. Build and upload
Plug in the CYD over USB, then in PlatformIO: **Build**, then **Upload**. From a terminal:

```bash
pio run -t upload
pio device monitor        # 115200 baud
```

### 4. In the truck
Plug into the OBD-II port, turn the key on (engine running for the best data) and power the
CYD. The status bar shows `NO CAN REPLY - DEMO DATA - RETRY Ns` until the PCM answers, then
`MONITOR ACTIVE - TAP TO SWIPE`. With the key off the PCM goes quiet and the display falls back
to demo data after 5 missed replies.

## Listen-only sniffer

The last page is a CAN sniffer for finding values the truck broadcasts on its own, such as
wheel speeds or the transfer case position, which are still unknown (see [Roadmap](#roadmap)).

- While the page is shown, the controller is in **listen-only mode**: it sends nothing, not even
  the ACK bit, so it can't disturb the bus. The status bar turns blue:
  `CAN LISTEN-ONLY - NOTHING SENT`. Leaving the page switches back to normal polling.
- Each row is one CAN ID: the ID, its latest data bytes and how many frames per second it is
  sent. Bytes that changed in the last second are **yellow**, so you can watch which bytes move
  when you spin a wheel, shift the transfer case or press the brake.
- **Tap the list** to show the next 10 IDs. The bottom row has three buttons:
  **REC / STOP** (SD recording, below), **MARK** and **NEXT >** (leave the page).
- Every 5 seconds the full ID list goes to the serial log (`CAN_SNIFF` lines), along with how
  many frames were missed if the receive queue filled up.

To boot straight onto the sniffer page, so nothing at all is sent until you tap away from it
(good for the first connection to the truck), flash the `sniffer` build:

```bash
pio run -e sniffer -t upload
```

## Battery protection

The OBD port's power pin is live with the key off, so the display powers itself down when the
truck goes quiet:

1. After **2 minutes** with no activity the backlight turns off. A tap turns it back on (that
   tap doesn't press anything on screen).
2. After **5 minutes** the ESP32 goes into **deep sleep** with the screen off. It wakes up on
   **CAN traffic** (the CAN RX pin going low as soon as any module talks, e.g. key on) or a
   **tap**, and starts up as if it had just been powered on.

"Activity" means the PCM answering requests, frames arriving on the sniffer page, or a tap. On
the bench with no truck, the display sleeps after 5 minutes too; tap to wake it. Both times can be
changed or turned off in [`src/config.h`](src/config.h).

Deep sleep stops the ESP32 and the screen, but the rest of the CYD board (voltage regulator,
USB-serial chip, power LED), the transceiver and a 12 V → 5 V converter still draw a few mA. Measure
it before leaving the display wired to always-on power. For zero drain, power it from an
ignition-switched fuse instead.

### Recording to the SD card

Put a FAT32 microSD card in the CYD's slot (it can go in while the display is running), open the
sniffer page and tap **REC**. The header turns red and shows the file name and how many frames
have been written, e.g. `REC CAN0003 12345`. Tap **STOP** when done; leaving the page or the
display going to sleep also stops it.

- Each recording is a new pair of files: `CAN0001.LOG`, `CAN0002.LOG`, ...
- **`CANnnnn.LOG`** has every frame in `candump` log format, one per line:
  `(0000123.456789) can0 4B0#1F861F8B1F901F95` (time in seconds since the CYD started, ID, data).
  Open it in [SavvyCAN](https://www.savvycan.com/) (File → Load Log File, type *candump*), or with
  python-can / cantools.
- **`CANnnnn.TXT`** holds the notes: the start time and a line for each **MARK** tap, e.g.
  `(0000051.516143) MARK 1`. Tap MARK the moment you do something (shift the transfer case,
  brake, turn the wheel), then look at the log around that time for the bytes that changed.
- The log is written to the card every 2 seconds, so unplugging the display loses at most the
  last 2 seconds.
- If the header shows `!` and a number, that many frames were dropped because the receive queue
  filled up (the card was too slow for a moment).
- `NO SD` on the button means no card was found; insert one and tap it again.

## Bench self-test (no transceiver)

The `selftest` build checks the CAN code on the bench without a transceiver or a truck:

```bash
pio run -e selftest -t upload
```

- The CAN controller runs in no-ACK self-test mode with its TX looped back to RX **inside the
  ESP32** (no jumper wire), so it receives every frame it sends.
- A simulated truck in [`src/can_simulator.h`](src/can_simulator.h) answers each request by
  sending real reply frames through the controller. Replies go through the same receive queue,
  filter and ISO-TP code as in the truck, including a multi-frame reply (7 stored codes), a
  "response pending" delay (trans temp) and a negative reply for unknown PIDs.
- The gauges sweep through changing values, the codes page lists 7 codes, the ABS page shows
  wheel speeds and 1 ABS code, and the sniffer page lists 15 fake broadcast IDs.
- Every 90 seconds the simulated key goes off for 15 seconds, to test falling back to demo data
  and reconnecting.
- The status bar starts with `SIM:` so the values can't be mistaken for real ones.

## Configuration

Everything you're likely to change is in [`src/config.h`](src/config.h):

| Setting | Default | What it does |
|---|---|---|
| `CAN_TX_PIN` / `CAN_RX_PIN` | 22 / 27 | GPIO pins to the transceiver's CTX / CRX |
| `POLL_INTERVAL_MS` | 250 | How often the visible values are refreshed |
| `DTC_SCAN_INTERVAL_MS` | 10000 | How often engine codes are re-checked when not on the codes page |
| `CAN_RETRY_INTERVAL_MS` | 5000 | How often the PCM is probed while it isn't answering |
| `CAN_REPLY_TIMEOUT_MS` | 100 | How long to wait for a module's reply |
| `CAN_MAX_MISSED_REPLIES` | 5 | PCM requests in a row without a reply before falling back to demo |
| `SCREEN_OFF_AFTER_MS` | 120000 | Backlight off after this long with no activity (0 = never) |
| `SLEEP_AFTER_MS` | 300000 | Deep sleep after this long with no activity (0 = never) |
| `MAX_SNIFF_IDS` | 64 | Most CAN IDs the sniffer keeps track of |
| `SNIFF_LOG_INTERVAL_MS` | 5000 | How often the sniffer logs its ID list to serial |
| `SNIFF_HIGHLIGHT_MS` | 1000 | How long a changed byte stays yellow on the sniffer page |
| `LOG_FLUSH_INTERVAL_MS` | 2000 | How often an SD recording is saved to the card |
| `MENU_TIMEOUT_MS` | 4000 | How long the tap menu stays up |
| `TOUCH_RAW_*` | 200–3700 / 240–3800 | Touch calibration (the defaults work on the tested board) |

**Adding or moving a gauge:** edit the `metrics[]` table in
[`src/display_ui.h`](src/display_ui.h). Each row is a label, value, unit, colour and the
function that reads it, and the pages are filled 4 at a time in table order. Only gauges on
screen are polled, so the table is the only place you need to change.

## Project structure

```
CYD-OBDII-CAN/
├── platformio.ini       Board, display pin setup (TFT_eSPI build flags), libraries, sniffer / self-test builds
├── src/
│   ├── config.h         Pins, tuning constants, shared declarations
│   ├── touch_bitbang.h  XPT2046 touch driver (bit-banged, frees the SPI bus for the SD card)
│   ├── globals.cpp      Shared objects and values, demo values
│   ├── main.cpp         setup() / loop(): touch, refresh timing, connect/disconnect handling
│   ├── obd_engine.h     CAN (TWAI + ISO-TP) requests, PID decoding, DTC reading
│   ├── can_sniffer.h    Listen-only sniffer: per-ID table, change tracking, serial summary
│   ├── can_logger.h     SD card recording (candump .LOG + MARK notes .TXT)
│   ├── can_simulator.h  Simulated truck for the bench self-test build
│   ├── power.h          Battery protection: screen off, deep sleep, wake on CAN or tap
│   └── display_ui.h     Gauge table, cards, full-screen gauge, tap menu, status bar, pages
├── hardware/
│   └── case/            3D-printable case (STL files, credits, build notes)
├── tools/
│   └── obd_logger.py    PC script that surveys the truck through a USB ELM327 and writes a log
├── docs/                Reference captures from the truck
└── test/
    └── serialLog.txt    Serial log captured in the truck (used to fix reply timing and FICM scaling)
```

## PC logging tool

[`tools/obd_logger.py`](tools/) is a Python script for a PC with a USB ELM327 adapter. It finds
the protocol and every module that answers, lists each module's supported PIDs, reads and
decodes them, measures response times, and writes everything, including a raw transcript, to one
text file. Use the log to choose the PIDs and formulas for the firmware. It only sends read requests.

```bash
pip install pyserial
python tools/obd_logger.py --note "engine idling, warm"
```

See [`tools/README.md`](tools/README.md) for the options and how to add Ford-specific PIDs.

## How it works

- **CAN transport:** the ESP32's TWAI controller runs at 500 kbps with 11-bit IDs. Each request
  is an ISO-TP single frame to the module (e.g. `7E0`) and the reply comes from that ID + 8
  (`7E8`). Long replies, such as many stored codes, arrive as a First Frame plus Consecutive
  Frames; the firmware sends a Flow Control frame and joins them. Replies are turned into the
  same hex text the ELM327 gave (e.g. `410C0AD6`), so the PID decoders are unchanged from
  CYD-OBDII.
- **Receive filter:** in normal mode only IDs `700`–`7FF` (diagnostic replies) are accepted, so
  the truck's normal broadcast traffic never fills the receive queue. The sniffer reinstalls the
  driver in listen-only mode with every ID accepted and a 128-frame queue.
- **Bus errors:** if a request is never acknowledged (nothing on the bus) or the controller goes
  bus-off, it is restarted before the next request.
- **Drawing:** the gauge area is a 320×214 8-bit sprite (about 67 KB) drawn off-screen and
  pushed in one go, so updates don't flicker. Page changes slide horizontally.
- **Debug log:** everything is logged over USB serial at 115200 with timestamps, e.g.
  `7E0 010C -> 410C0AD6` or `Page 0 refreshed in 20 ms`.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| Stuck on `NO CAN REPLY` | Key off; CAN-H/CAN-L swapped; CTX/CRX swapped; no ground to OBD pin 5. The log shows `No ACK on the bus` when nothing on the bus acknowledges the frames |
| `CAN DRIVER FAILED - DEMO DATA` | The TWAI driver didn't start; power-cycle the CYD |
| A gauge stays at `---` | The truck doesn't answer that PID; check the serial log for `no valid reply` |
| Taps hit the wrong gauge | Recalibrate `TOUCH_RAW_*` using the `Tap -> RawX/RawY` lines in the serial log |
| Values drop out or the log shows `Bus-off` | Extra 120 Ω terminator on the transceiver board, or long / untwisted CAN wires |

## Roadmap

- [ ] Find the transfer case module and its address with FORScan (it may be on MS-CAN)
- [ ] Real ABS wheel-speed PIDs and scaling
- [ ] Confirm coolant temp, trans temp and DTC reads in a new truck log
- [x] Hard-wire the CYD to the CAN bus instead of using the Bluetooth ELM327 adapter
      (TWAI + SN65HVD230; code done, **not yet tested in the truck**)
- [x] Listen-only sniffer page to find broadcast values without sending requests
      (**not yet tested on a bus**)
- [x] Record sniffer traffic to the SD card with markers (tested on the bench with the self-test build)
- [ ] Use the sniffer and SD recordings to find wheel-speed and transfer-case broadcasts
- [ ] Measure the key-off current draw in deep sleep (screen-off and deep-sleep firmware are done
      and tested on the bench; waking on CAN traffic still needs a transceiver to test)

## Contributing

Ideas, bug reports and pull requests are welcome.

- **Know a Ford PID or module address?** The open [`help wanted`](../../issues?q=is%3Aissue+is%3Aopen+label%3A%22help+wanted%22)
  issues list what's still unknown (transfer case, ABS wheel speeds, temperature PIDs). Log
  output from [`tools/obd_logger.py`](tools/) or FORScan is the most useful thing you can add.
- **Code feedback:** open an issue or start a thread in Discussions. The CAN request handling in
  [`src/obd_engine.h`](src/obd_engine.h) and the drawing code in
  [`src/display_ui.h`](src/display_ui.h) are good places to start.
- Please only add **read-only** requests (see Safety below).

## Safety

This project only **reads** data (services `01`, `03`, `22`, `18`). Don't add requests that
command modules (services `2E`, `2F`, `31`, `10`), since those can actuate hardware or change
settings. Mount the display where it doesn't block your view, and don't operate it while
driving.

## License

The code is released under the [MIT License](LICENSE). The 3D-printable case in
[`hardware/case/`](hardware/case/) is a remix of designs by DanielBrunod and ncmreynolds and stays
under **CC BY-SA**. See [hardware/case/README.md](hardware/case/README.md).
