#include <Arduino.h>
#include <SPI.h>
#include "config.h"
#include "obd_engine.h"
#include "can_sniffer.h"
#include "display_ui.h"
#include "power.h"

void setup() {
    Serial.begin(115200);
    delay(500);
    logDebug("SYS_INIT", "Powering up ESP32 core subsystems...");
    powerBegin();

    pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
    digitalWrite(TFT_BACKLIGHT_PIN, HIGH);

    tft.init();
    tft.invertDisplay(true);
    tft.setRotation(1);
    drawSplashScreen();
    delay(2500); // 2.5 second hold so the display text is readable
    tft.fillScreen(TFT_BLACK);

    // Touch uses the default SPI object (VSPI); the TFT is on HSPI via -D USE_HSPI_PORT
    SPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
    touch.begin();
    touch.setRotation(1);
    logDebug("TOUCH_INIT", "XPT2046 Driver successfully initialized.");

    canvas.setColorDepth(8);   // 8-bit sprite: 320x214 = ~67 KB instead of ~137 KB
    if (canvas.createSprite(320, CANVAS_H) == nullptr) {
        logDebug("GFX_FATAL", "Canvas sprite allocation failed (out of RAM)!");
    }

    // Start in demo mode: default values on screen, touch + paging fully usable
    loadDemoValues();

    // The "sniffer" build (pio run -e sniffer) starts on the listen-only page, so the
    // first connection to a bus sends nothing at all
#ifdef CAN_BOOT_LISTEN_ONLY
    const bool listenOnly = true;
    currentPage = PAGE_SNIFF;
#else
    const bool listenOnly = false;
#endif

    logDebug("CAN_INIT", "Starting TWAI driver at 500 kbps (TX " + String(CAN_TX_PIN) + ", RX " + String(CAN_RX_PIN) + ")" +
                         (listenOnly ? " in LISTEN-ONLY mode..." : "..."));
    if (!canBegin(listenOnly)) {
        // Not fatal: keep running in demo mode so the UI can still be tested
        logDebug("CAN_FATAL", "TWAI driver failed to start! Staying in demo mode.");
        canDriverFailed = true;
    } else if (!listenOnly) {
        tryCanConnect();   // a probe takes ~100 ms, so the first one runs right away
        lastCanRetry = millis();
    }

    updateStatusBar(true);
    drawCurrentScreen();
    noteActivity();   // the idle timers start once the screen is up
}

void loop() {
    static bool wasConnected = false;
    static unsigned long lastBarCheck = 0;

    // 0. The sniffer page puts the controller in listen-only mode; any other page polls.
    //    While sniffing, every waiting frame is read on each pass so the queue can't fill.
    bool wantListenOnly = currentPage == PAGE_SNIFF;
    if (wantListenOnly != canListenOnly && !canDriverFailed) {
        canSetListenOnly(wantListenOnly);
        if (wantListenOnly) sniffReset();
        updateStatusBar(true);
    }
#ifdef CAN_SELF_TEST
    simTick(canListenOnly);   // fake broadcasts for the sniffer, simulated key on/off
#endif
    if (canListenOnly) {
        static uint32_t lastSniffTotal = 0;
        sniffPoll();
        if (sniffTotal != lastSniffTotal) noteActivity();   // the bus is awake
        lastSniffTotal = sniffTotal;
    }

    //    Connect / detect a dropped link: the PCM stops answering when the key is
    //    turned off, so several missed replies in a row means it's gone
    if (isConnected && missedPcmReplies >= CAN_MAX_MISSED_REPLIES) {
        logDebug("CAN_WARN", "PCM stopped answering. Falling back to demo mode.");
        isConnected = false;
        lastCanRetry = millis();
    }
    if (!isConnected && !canDriverFailed && !canListenOnly && millis() - lastCanRetry > CAN_RETRY_INTERVAL_MS) {
        tryCanConnect();
        lastCanRetry = millis();
    }
    if (isConnected && !canListenOnly) noteActivity();   // PCM answering: the truck is on

    // 1. React to connect / disconnect transitions
    if (isConnected != wasConnected) {
        wasConnected = isConnected;
        if (isConnected) clearLiveValues();   // don't show demo numbers as if they were real
        else             loadDemoValues();
        lastBusQuery = 0;                     // force an immediate refresh
        updateStatusBar(true);
        drawCurrentScreen();
    }

    // 2. Touch: one action per tap (finger must lift before the next tap counts, so a
    //    held finger can't hit a menu button that appears under it)
    static bool wasTouched = false;
    bool touchedNow = touch.touched();
    if (touchedNow && !wasTouched && millis() - lastTouchDebounce > 250) {
        lastTouchDebounce = millis();
        TS_Point pt = touch.getPoint();
        int sx = constrain(map(pt.x, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, 319), 0, 319);
        int sy = constrain(map(pt.y, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, 239), 0, 239);
        logDebug("TOUCH_ACT", "Tap -> RawX: " + String(pt.x) + ", RawY: " + String(pt.y) +
                              " | Screen: " + String(sx) + "," + String(sy));
        if (screenIsOff) noteActivity();   // this tap only wakes the screen
        else { noteActivity(); handleTap(sx, sy); }
    }
    wasTouched = touchedNow;

    // Close the tap menu if nothing was pressed
    if (menuMetric >= 0 && millis() - menuShownAt > MENU_TIMEOUT_MS) {
        menuMetric = -1;
        drawCurrentScreen();
    }

    // 3. Refresh data + page every POLL_INTERVAL_MS (live data only when connected)
    if (millis() - lastBusQuery > POLL_INTERVAL_MS) {
        lastBusQuery = millis();
        if (isConnected && !canListenOnly) updateVehicleData();
        drawCurrentScreen();
    }

    // 4. Keep the status bar current (retry countdown, DTC warning); repaints only on change
    if (millis() - lastBarCheck > 250) {
        lastBarCheck = millis();
        updateStatusBar();
    }

    // 5. Battery protection: screen off, then deep sleep, when nothing is happening
    powerTick();
}