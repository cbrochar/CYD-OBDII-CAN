#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include "driver/twai.h"
#include "touch_bitbang.h"

// Hardware Pin Definitions for the Cheap Yellow Display (CYD)
// Touch (XPT2046) is bit-banged so the SD card can have the second hardware SPI bus
#define XPT2046_IRQ   36
#define XPT2046_MOSI  32
#define XPT2046_MISO  39
#define XPT2046_CLK   25
#define XPT2046_CS    33

// CAN bus (ESP32 built-in TWAI controller) through a 3.3 V SN65HVD230 transceiver.
// Both pins are on the CYD's CN1 connector (GND, IO22, IO27, 3.3V), which also powers it.
// RX must be an RTC-capable pin (IO27 is, IO22 isn't) so bus traffic can wake deep sleep.
#define CAN_TX_PIN    22   // ESP32 TX -> transceiver D (CTX)
#define CAN_RX_PIN    27   // ESP32 RX <- transceiver R (CRX)

// MicroSD slot (VSPI). Cards must be FAT32 (32 GB or smaller is simplest).
#define SD_SCK_PIN    18
#define SD_MISO_PIN   19
#define SD_MOSI_PIN   23
#define SD_CS_PIN     5

// Touch calibration: raw XPT2046 readings at the screen edges (rotation 1).
// Typical CYD values; if taps land on the wrong card, tap each corner and copy the
// RawX/RawY numbers from the TOUCH_ACT serial log into these four constants.
const int TOUCH_RAW_X_MIN = 200,  TOUCH_RAW_X_MAX = 3700;
const int TOUCH_RAW_Y_MIN = 240,  TOUCH_RAW_Y_MAX = 3800;

// How long the tap menu (NEXT PAGE / FULL SCREEN) stays up if nothing is pressed
const unsigned long MENU_TIMEOUT_MS = 4000;

// The gauge area is drawn off-screen in CANVAS_STRIPS horizontal strips, each pushed to
// the screen in turn. Fewer strips draw faster, more strips need less RAM. Here:
// one full-size canvas: this version has RAM to spare.
const int CANVAS_STRIPS = 1;

// Display layout / paging
const int METRICS_PER_PAGE = 4;   // 2 x 2 grid of cards
const int METRIC_PAGES     = 3;   // card pages; must match the metric table in display_ui.h (12 metrics)
const int TOTAL_PAGES      = 7;   // metric pages + DTC list + drive log + settings + CAN sniffer
const int PAGE_ABS         = 2;   // ABS wheel speeds (only polled while this page is shown)
const int PAGE_DTC         = 3;   // stored trouble code list
const int PAGE_LOG         = 4;   // drive log to the SD card (REC / MARK)
const int PAGE_SETTINGS    = 5;   // settings (saved in flash)
const int PAGE_SNIFF       = 6;   // listen-only CAN sniffer (nothing is sent while it's shown)

// Stored trouble codes kept for the DTC page (3 columns x 5 rows); pending codes too
const int MAX_DTCS = 15;

// Gauge alarms: outside these limits a gauge flashes red and the status bar shows it on
// every page. Gauges with a limit are also read in the background (every
// ALARM_CHECK_INTERVAL_MS) while they're off screen. Which gauge uses which limit is set
// in the metric table in display_ui.h; NO_ALARM there means no limit.
// These limits are the defaults: they can be changed on the Settings page.
#define NO_ALARM nullptr
const float ALARM_FICM_LOW_V      = 45.0;    // FICM main power below this (nominal ~48 V)
const float ALARM_TFT_HIGH_F      = 220;     // transmission fluid above this
const float ALARM_ECT_HIGH_F      = 230;     // coolant above this
const float ALARM_VOLTS_LOW_V     = 11.8;    // system voltage below this (not charging)
const float ALARM_VOLTS_HIGH_V    = 15.0;    // system voltage above this (overcharging)
const unsigned long ALARM_CHECK_INTERVAL_MS = 2000;
const unsigned long ALARM_BLINK_MS          = 500;

// Live data refresh: the visible page is re-read this often (a refresh takes roughly
// 100 ms per value, so in practice the page updates as fast as the truck answers)
const unsigned long POLL_INTERVAL_MS     = 250;
// Engine trouble codes are re-scanned this often when not on the DTC page
const unsigned long DTC_SCAN_INTERVAL_MS = 10000;

// While the PCM isn't answering (key off, not plugged in), it is probed this often
const unsigned long CAN_RETRY_INTERVAL_MS = 5000;
// How long to wait for a module's reply to one request
const unsigned long CAN_REPLY_TIMEOUT_MS  = 100;
// This many PCM requests in a row without a reply counts as the link being lost
const int CAN_MAX_MISSED_REPLIES = 5;

// Auto brightness (brightness.h): the light sensor sets the backlight between
// BRIGHTNESS_MIN_PCT (reading at LDR_DARK_RAW or darker) and 100% (at LDR_BRIGHT_RAW).
// To recalibrate, set LIGHT_LOG_INTERVAL_MS and read the LIGHT lines in the serial log.
#define LDR_PIN       34
const bool AUTO_BRIGHTNESS                = true;    // default; false = always full brightness
const int  BRIGHTNESS_MIN_PCT             = 10;      // default night level (Settings page)
const int  LDR_DARK_RAW                   = 2000;    // the CYD sensor reads HIGHER in the dark: ~2600 fully dark, 100-350 shaded
const int  LDR_BRIGHT_RAW                 = 0;       // and 0 in normal room light or brighter
const unsigned long LIGHT_SMOOTHING_MS    = 3000;
const unsigned long LIGHT_LOG_INTERVAL_MS = 0;       // e.g. 500 to log readings for calibration

// Battery protection (power.h): with no activity (PCM answering, sniffer traffic or a
// tap) the backlight goes off, then the board deep-sleeps until CAN traffic or a tap.
// 0 turns that step off.
const unsigned long SCREEN_OFF_AFTER_MS = 2UL * 60 * 1000;
const unsigned long SLEEP_AFTER_MS      = 5UL * 60 * 1000;

// Sniffer page: most IDs tracked, and how often the full ID list is logged to serial
const int MAX_SNIFF_IDS                  = 64;
const unsigned long SNIFF_LOG_INTERVAL_MS = 5000;
// Bytes that changed within this time are highlighted on the sniffer page
const unsigned long SNIFF_HIGHLIGHT_MS    = 1000;
// While recording to SD, the log is flushed this often (a power cut loses at most this much)
const unsigned long LOG_FLUSH_INTERVAL_MS = 2000;

// Drive log (drive_log.h): with REC on the LOG page, the gauges are written to the SD card
// this often, and the file is saved to the card this often (a power cut loses at most that)
const unsigned long DRIVE_LOG_INTERVAL_MS = 1000;
const unsigned long DRIVE_LOG_FLUSH_MS    = 5000;

// Phone hotspot (wifi_portal.h), switched on from the Settings page. Join this network on a
// phone and open http://192.168.4.1 for live gauges, codes, SD logs and firmware updates.
// The password must be at least 8 characters.
#define WIFI_AP_SSID      "CYD-OBDII"
#define WIFI_AP_PASSWORD  "powerstroke"

// Global Shared Subsystem Instances
extern TFT_eSPI tft;
extern TFT_eSprite canvas;
extern TouchBitbang touch;

// Program Tracking States
extern bool isConnected;             // true while the PCM answers requests
extern bool canDriverFailed;         // true if the TWAI driver failed to start
extern unsigned long lastBusQuery;
extern unsigned long lastTouchDebounce;
extern unsigned long lastCanRetry;
extern int currentPage;
extern int menuMetric;              // metric whose tap menu is showing (-1 = none)
extern unsigned long menuShownAt;
extern int fullScreenMetric;        // metric shown full screen (-1 = normal pages)

// Shared Metric Buffers
extern String valTCase;
extern String valBoost;
extern String valFICM;
extern String valECT;
extern String valTFT;
extern String valVolts;
extern String valRPM;
extern String valSpeed;
extern String valWheelLF;
extern String valWheelRF;
extern String valWheelRear;        // single rear-axle (differential) speed sensor
extern bool hasDTCWarning;
extern String dtcCodes[MAX_DTCS];   // decoded codes, e.g. "P0470"
extern int dtcTotal;                // codes reported by the ECU (-1 = not read yet)
extern String pendingCodes[MAX_DTCS];   // pending codes (Mode 07): detected once, not yet confirmed
extern int pendingTotal;            // -1 = not read yet
extern int absDtcTotal;             // codes reported by the ABS module (-1 = not read yet)

// Fill all metrics with demo values (used while the truck isn't answering)
void loadDemoValues();
// Blank all metrics to "---" (used when a real link comes up, until real data arrives)
void clearLiveValues();

// Reusable Timestamped Debug Logger
inline void logDebug(String system, String message) {
    Serial.printf("[%010lu] (%s): %s\n", millis(), system.c_str(), message.c_str());
}

#endif