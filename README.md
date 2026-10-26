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

- **6 pages**, tap to move between them (page dots at the bottom show where you are)
  1. Transfer case · Turbo boost · Vehicle speed · Transmission temp
  2. FICM main power · Coolant temp · System volts · Engine RPM
  3. ABS: Left-front wheel speed · Right-front wheel speed · Rear axle speed · ABS status (warning triangle when the ABS module has stored codes)
  4. Engine trouble codes (up to 15, e.g. `P0470`): stored codes in red, then pending codes in
     yellow (detected once but not confirmed yet, so they show up before the check-engine light);
     green when clear. **Tap a code** to see what it means.
  5. **Settings**: brightness, units, alarm limits, power and WiFi, saved in flash, see
     [Settings page](#settings-page)
  6. **CAN sniffer** (listen-only) with **SD card recording**, see [Listen-only sniffer](#listen-only-sniffer)
- **Tap menu**: tap any gauge to get **NEXT PAGE** and **FULL SCREEN** buttons.
  Full screen shows that one gauge as large as it fits; tap again to go back.
- **Gauge alarms**: a gauge that goes out of range (low FICM voltage, hot transmission or
  coolant, low or high system voltage) flashes red, and the status bar shows it on every page,
  see [Gauge alarms](#gauge-alarms).
- **Status bar** that never scrolls: connection state, demo mode, retry countdown, gauge alarms,
  a red warning when engine trouble codes are stored and a yellow one for pending codes.
- **Demo mode**: runs on sample values whenever the PCM isn't answering (key off, not plugged
  in), and probes it again every 5 seconds.
- **Phone access over WiFi**: switch on the CYD's own hotspot to see the live gauges and codes on
  your phone, download SD recordings and update the firmware without a cable, see
  [Phone access (WiFi)](#phone-access-wifi).
- **Auto brightness**: the CYD's light sensor dims the screen at night and brings it back to full
  brightness in daylight, smoothly, see [Auto brightness](#auto-brightness).
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

### From Ford's workshop manual (not yet confirmed on the truck)

A forum member checked the 2005 Excursion Workshop Manual, section 418-00 (Module
Communications Network):

- The **ABS module** and the **4WD control module** (Electronic Shift-on-the-Fly) talk over
  **ISO 9141, the K-line on OBD pin 7**, not HS-CAN or MS-CAN. That's why nothing answers at the
  CAN addresses tried so far, and why the CAN sniffer won't find wheel speeds or the transfer
  case. Reading them needs K-line access (see [Roadmap](#roadmap)). The PC logger's `--kline`
  probe finds which K-line addresses answer.
- **4WD module PIDs** (Ford names, numeric IDs not known yet): `4WD_SW` = selector position
  (`2WD` / `4WD` / `4WD LOW`, the *requested* position), `PLATE_A`–`PLATE_D` = shift-motor
  encoder contacts (the *actual* position), `VSS2` = vehicle speed input. Ford's Pinpoint Test C32:

  | Contact | 2H | 4H | 4L |
  |---|---|---|---|
  | PLATE_A | open | closed | closed |
  | PLATE_B | open | open | closed |
  | PLATE_C | closed | closed | open |
  | PLATE_D | open | open | open |

  Comparing `4WD_SW` with the plates also shows when a shift is still in progress.
- **ABS module PIDs** (from older Excursion documentation, may differ for 2005): `LF_WSPD`,
  `RF_WSPD`, `R_WSPD` (wheel speeds, 0–255 km/h; one rear-axle sensor), `CCNTABS` (number of
  stored ABS codes), `ABSLAMP`, `BRKLAMP`, `BOO_ABS` (brake pedal switch), `4WDABS`, inlet/outlet
  valve states and `PWR_RLY`. Ford ABS codes include `C1155` (LF wheel speed sensor), `C1145` (RF)
  and `C1230` (rear axle).

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
| Pending trouble codes | PCM `7E0` | `07` | CAN Mode 07, same format as Mode 03 | ⚠️ Not yet confirmed |
| Transfer case | 4WD module, **K-line** | `4WD_SW`, `PLATE_A`–`D` | see the table above | ❌ Not on CAN: needs K-line access (code still tries `418F02`, which returns NO DATA) |
| Wheel speeds (LF/RF/rear) | ABS module, **K-line** | `LF_WSPD`, `RF_WSPD`, `R_WSPD` | 0–255 km/h | ❌ Not on CAN: needs K-line access (code still tries placeholders at `760`) |
| ABS trouble codes | ABS module, **K-line** | `CCNTABS` count | | ❌ Not on CAN: needs K-line access |

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

## Trouble code descriptions

On the codes page, **tap a code** to open a box with what it means, e.g. `P0470` → "Exhaust back
pressure (EBP) sensor circuit". Pending codes also explain that they've been seen once but aren't
confirmed yet. Tap anywhere to close the box.

The list in [`src/dtc_descriptions.h`](src/dtc_descriptions.h) covers the codes common on the
6.0L Powerstroke (EBP, ICP, FICM, EGR, glow plugs, turbo, injectors by cylinder) plus generic
OBD-II codes and the Ford ABS wheel-speed codes. A code that isn't listed says whether it's a
generic or a Ford-specific code. To add one, add a row: `{"P0123", "What it means"},`.

## Settings page

The settings page (after the codes page) changes these without editing code. Changes are saved
in the ESP32's flash 2 seconds after the last tap, so they survive power-off and firmware uploads.

| Section | Setting | Default |
|---|---|---|
| DISPLAY | Auto brightness on/off, night brightness (5–100%), temperature °F/°C, speed MPH/KM/H | on, 10%, °F, MPH |
| ALARMS | Gauge alarms on/off, FICM low, trans temp high, coolant high | on, 45.0 V, 220 °F, 230 °F |
| VOLTAGE | System volts low / high alarm | 11.8 V / 15.0 V |
| POWER | Screen off after (never, 1–30 min), sleep after (never, 5–60 min) | 2 min, 5 min |
| WIFI | Phone hotspot on/off, plus the network name, password and address | off |

- Each row has one button (tap to switch) or **−** / **+** buttons.
- **MORE** goes to the next section, **NEXT >** to the next page.
- **DEFAULTS** puts everything back to the defaults: tap it, it turns red (`SURE?`), then tap
  again within 3 seconds.
- The defaults are the values in [`src/config.h`](src/config.h). Temperatures are kept in °F
  and speeds in MPH internally; the units settings only change what's shown, so alarm limits
  stay the same whichever units you pick.

## Phone access (WiFi)

The CYD can run its own WiFi hotspot, so a phone can see the dash without any app:

1. On the CYD: Settings → **MORE** to **WIFI** → **Phone hotspot ON**.
2. On the phone, join the WiFi network **`CYD-OBDII`**, password **`powerstroke`** (set
   `WIFI_AP_SSID` / `WIFI_AP_PASSWORD` in [`src/config.h`](src/config.h)). If the phone warns
   that the network has no internet, choose to stay connected.
3. Open **http://192.168.4.1** in the phone's browser.

| Tab | What it does |
|---|---|
| **Live** | All gauges, refreshed every second, in the units chosen on the CYD; alarms in red; stored and pending codes with their descriptions |
| **Logs** | The SD card recordings (`.LOG` and `.TXT`): download to the phone or delete. The file being recorded is shown but can't be downloaded until you STOP |
| **Update** | Upload a new firmware `.bin` (from `.pio/build/esp32dev/firmware.bin` after a PlatformIO build). The CYD restarts with it when the upload is checked |

- The phone only sees what the display already reads: no extra requests go to the truck.
- The hotspot is **off by default** because it draws about 100 mA; the setting is remembered.
  While a phone is using the page the display stays awake.
- Anyone in range who knows the password can use the page, including the firmware update. Change
  the password in `config.h` if that matters to you.

## Listen-only sniffer

The last page is a CAN sniffer for finding values the truck broadcasts on its own on HS-CAN.
(Per Ford's workshop manual the ABS and 4WD modules are on the K-line, not CAN, so wheel speeds
and the transfer case position won't show up here; see
[From Ford's workshop manual](#from-fords-workshop-manual-not-yet-confirmed-on-the-truck).)

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
- To find out which bytes changed at a MARK or between two recordings, use
  [`tools/can_compare.py`](tools/README.md#can_comparepy) on the PC.

## Gauge alarms

A gauge whose value goes outside its limits gets a red frame and flashes solid red, and the
status bar turns red with the reading, e.g. `[!] TRANS TEMP HIGH: 224 F`, on whatever page is
showing. Gauges with limits are also read in the background every 2 seconds while they're on
another page, so an alarm is never missed. Each alarm and its clearing are logged to serial
(`ALARM` lines). Alarms only use live readings, never demo data.

| Gauge | Alarm when (default) | Default in [`src/config.h`](src/config.h) |
|---|---|---|
| FICM main power | below 45.0 V | `ALARM_FICM_LOW_V` |
| Trans temp | above 220 °F | `ALARM_TFT_HIGH_F` |
| Coolant temp | above 230 °F | `ALARM_ECT_HIGH_F` |
| System volts | below 11.8 V or above 15.0 V | `ALARM_VOLTS_LOW_V`, `ALARM_VOLTS_HIGH_V` |

Change the limits, or switch alarms off, on the [Settings page](#settings-page) (ALARMS and
VOLTAGE); `config.h` only holds the defaults. To add a limit to another gauge, or remove one, edit
the last two columns (low limit, high limit) of its row in the `metrics[]` table in
[`src/display_ui.h`](src/display_ui.h); they point at a limit value, and `NO_ALARM` means no limit.
For example, to warn above 3,200 RPM:

```cpp
static const float RPM_HIGH = 3200;   // above the metrics[] table
{"ENGINE RPM",     &valRPM,    "",    TFT_WHITE,   pollRPM,    NO_ALARM, &RPM_HIGH},
```

`ALARM_CHECK_INTERVAL_MS` (2000) sets how often off-screen gauges are checked, and
`ALARM_BLINK_MS` (500) the flash speed.

## Auto brightness

The light sensor on the front of the CYD (an LDR on GPIO34) sets the backlight brightness.

- On this board the sensor reads **0 in normal room light or brighter**, rises to 100–350 in
  shade and to about 2,600 in full darkness (higher = darker).
- 0 gives 100% brightness; a reading of 2,000 or more gives the 10% night floor. The curve is
  squared, so dusk and shade only dim the screen a little and it drops off near full darkness.
- The reading is smoothed over about 3 seconds, so a passing shadow or headlights don't make the
  screen flicker.

### Changing the brightness levels

**Auto brightness on/off** and the **night brightness** are on the [Settings page](#settings-page)
(DISPLAY). The rest are in the "Auto brightness" block of [`src/config.h`](src/config.h); after
changing one, upload again (`pio run -t upload`, or `-e sniffer` / `-e selftest` for those builds).

| Setting | Default | What it does |
|---|---|---|
| `AUTO_BRIGHTNESS` | `true` | Default for the Settings page switch (`false` = always 100%) |
| `BRIGHTNESS_MIN_PCT` | `10` | Default night brightness, in % |
| `LDR_DARK_RAW` | `2000` | Sensor reading at which the screen reaches the night brightness |
| `LDR_BRIGHT_RAW` | `0` | Sensor reading at which the screen is at 100% |
| `LIGHT_SMOOTHING_MS` | `3000` | How quickly the brightness follows the light, in ms (higher = slower and steadier) |
| `LIGHT_LOG_INTERVAL_MS` | `0` | How often to log sensor readings, in ms (`0` = off; use `500` for calibration) |

Common adjustments:

- **Too dim at night / too bright at night:** Settings page → DISPLAY → Night brightness.
- **Dims too early (at dusk or in shade):** raise `LDR_DARK_RAW`, e.g. `2400`; the screen then
  only dims in deeper darkness.
- **Doesn't dim enough at night:** lower `LDR_DARK_RAW`, e.g. `1000`, so a less dark cab already
  counts as full darkness.
- **Brightness hunts or flickers** (streetlights, shadows): raise `LIGHT_SMOOTHING_MS`, e.g.
  `6000`. **Reacts too slowly** (tunnels): lower it, e.g. `1500`.

### Recalibrating for your board

The sensor's range differs between CYD boards, so if the defaults don't suit yours:

1. In [`src/config.h`](src/config.h), set `LIGHT_LOG_INTERVAL_MS = 500` and upload.
2. Open the serial monitor (`pio device monitor`). Twice a second it logs a line like
   `(LIGHT): LDR raw 2587, smoothed 2555 -> backlight 10%`. `raw` is the sensor reading.
3. Note the `raw` value in each lighting condition you care about: daylight, the cab at dusk,
   the cab at night with the dash lights on, and fully covered.
4. Set `LDR_BRIGHT_RAW` to the reading where you still want 100% (usually `0`) and
   `LDR_DARK_RAW` to the reading where you want the night level.
5. Set `LIGHT_LOG_INTERVAL_MS` back to `0` and upload again.

On the board this was developed on, the readings were 0 in room light, 100–350 in shade and
about 2,600 fully dark. Higher always means darker on the CYD.

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
changed or turned off on the [Settings page](#settings-page) (POWER).

Deep sleep stops the ESP32 and the screen, but the rest of the CYD board (voltage regulator,
USB-serial chip, power LED), the transceiver and a 12 V → 5 V converter still draw a few mA. Measure
it before leaving the display wired to always-on power. For zero drain, power it from an
ignition-switched fuse instead.

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
- The gauges sweep through changing values (FICM, coolant and trans temp cross their alarm
  limits now and then), the codes page lists 7 stored codes and 1 pending code, the ABS page
  shows wheel speeds and 1 ABS code, and the sniffer page lists 15 fake broadcast IDs.
- Every 90 seconds the simulated key goes off for 15 seconds, to test falling back to demo data
  and reconnecting.
- The status bar starts with `SIM:` so the values can't be mistaken for real ones.

## Configuration

Brightness, units, alarm limits, the power timers and WiFi are on the
[Settings page](#settings-page). Everything else is in [`src/config.h`](src/config.h) (the
values marked "default" are what the Settings page starts with and goes back to):

| Setting | Default | What it does |
|---|---|---|
| `CAN_TX_PIN` / `CAN_RX_PIN` | 22 / 27 | GPIO pins to the transceiver's CTX / CRX |
| `POLL_INTERVAL_MS` | 250 | How often the visible values are refreshed |
| `DTC_SCAN_INTERVAL_MS` | 10000 | How often engine codes are re-checked when not on the codes page |
| `CAN_RETRY_INTERVAL_MS` | 5000 | How often the PCM is probed while it isn't answering |
| `CAN_REPLY_TIMEOUT_MS` | 100 | How long to wait for a module's reply |
| `CAN_MAX_MISSED_REPLIES` | 5 | PCM requests in a row without a reply before falling back to demo |
| `ALARM_*` | see [Gauge alarms](#gauge-alarms) | Default alarm limits for FICM, trans temp, coolant and system volts |
| `AUTO_BRIGHTNESS` | true | Default: set the backlight from the light sensor |
| `BRIGHTNESS_MIN_PCT` | 10 | Default night-time backlight level |
| `LDR_DARK_RAW` / `LDR_BRIGHT_RAW` | 2000 / 0 | Sensor readings for the night floor / full brightness |
| `SCREEN_OFF_AFTER_MS` | 120000 | Default: backlight off after this long with no activity |
| `SLEEP_AFTER_MS` | 300000 | Default: deep sleep after this long with no activity |
| `WIFI_AP_SSID` / `WIFI_AP_PASSWORD` | CYD-OBDII / powerstroke | The phone hotspot's network name and password |
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
│   ├── brightness.h     Auto brightness: light sensor -> backlight PWM
│   ├── settings.h       Settings page values, saved in flash (NVS)
│   ├── dtc_descriptions.h  What each trouble code means
│   ├── wifi_portal.h    Phone hotspot: live page, SD log downloads, firmware update
│   └── display_ui.h     Gauge table, cards, full-screen gauge, tap menu, status bar, pages
├── hardware/
│   └── case/            3D-printable case (STL files, credits, build notes)
├── tools/
│   ├── obd_logger.py    PC script that surveys the truck through a USB ELM327 and writes a log
│   └── can_compare.py   PC script that compares SD recordings to find which bytes carry a value
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

Add `--kline` to also probe the K-line (OBD pin 7) for the ABS and 4WD modules: it lists which
module addresses answer. See [`tools/README.md`](tools/README.md) for the options and how to add
Ford-specific PIDs.

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

- [ ] Confirm the ABS and 4WD modules are on the K-line (pin 7): run `obd_logger.py --kline` at the
      truck, or check FORScan's module list, which shows each module's bus
- [ ] Find the numeric PIDs for `4WD_SW`, `PLATE_A`–`D`, `LF_WSPD`, `RF_WSPD`, `R_WSPD`
- [ ] K-line access for the CAN version: an ISO 9141 transceiver (e.g. L9637D) on a free UART.
      The CYD has very few free pins (IO35 input-only on P3 could be RX; TX would need the RGB LED
      or speaker pin), so this needs some soldering
- [ ] Confirm coolant temp, trans temp and DTC reads in a new truck log
- [x] Hard-wire the CYD to the CAN bus instead of using the Bluetooth ELM327 adapter
      (TWAI + SN65HVD230; code done, **not yet tested in the truck**)
- [x] Listen-only sniffer page to find broadcast values without sending requests
      (**not yet tested on a bus**)
- [x] Record sniffer traffic to the SD card with markers (tested on the bench with the self-test build)
- [ ] Use the sniffer and SD recordings to see what the PCM broadcasts on HS-CAN (the ABS and
      4WD modules aren't on CAN, so their values won't be there)
- [ ] Test the WiFi firmware update (the Live and Logs pages are tested from a phone)
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

This project only **reads** data (services `01`, `03`, `07`, `22`, `18`). Don't add requests that
command modules (services `2E`, `2F`, `31`, `10`), since those can actuate hardware or change
settings. Mount the display where it doesn't block your view, and don't operate it while
driving.

## License

The code is released under the [MIT License](LICENSE). The 3D-printable case in
[`hardware/case/`](hardware/case/) is a remix of designs by DanielBrunod and ncmreynolds and stays
under **CC BY-SA**. See [hardware/case/README.md](hardware/case/README.md).
