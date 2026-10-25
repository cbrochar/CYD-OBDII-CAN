#ifndef POWER_H
#define POWER_H

#include <Arduino.h>
#include "config.h"
#include "can_logger.h"
#include "driver/rtc_io.h"
#include "esp_sleep.h"

// ---- Battery protection ------------------------------------------------------------
// The OBD port's power pin is live with the key off, so when the truck goes quiet the
// display steps down:
//   no activity for SCREEN_OFF_AFTER_MS -> backlight off (a tap turns it back on)
//   no activity for SLEEP_AFTER_MS      -> deep sleep, woken by CAN traffic or a tap
// "Activity" is the PCM answering, frames arriving on the sniffer page, or a tap.
// Deep sleep restarts the firmware on wake-up, like a power-on.

const int TFT_BACKLIGHT_PIN = 21;

static unsigned long lastActivityMs = 0;
static bool screenIsOff = false;

inline void setScreenOn(bool on) {
    if (on == !screenIsOff) return;
    screenIsOff = !on;
    digitalWrite(TFT_BACKLIGHT_PIN, on ? HIGH : LOW);
    logDebug("POWER", on ? "Screen on" : "No activity, screen off (tap to wake)");
}

inline void noteActivity() {
    lastActivityMs = millis();
    setScreenOn(true);
}

// Call first thing in setup(): releases the backlight pin held low during deep sleep
// and logs why the board woke up
inline void powerBegin() {
    gpio_hold_dis((gpio_num_t)TFT_BACKLIGHT_PIN);
    gpio_deep_sleep_hold_dis();
    switch (esp_sleep_get_wakeup_cause()) {
        case ESP_SLEEP_WAKEUP_EXT0: logDebug("POWER", "Woke from deep sleep: CAN bus activity"); break;
        case ESP_SLEEP_WAKEUP_EXT1: logDebug("POWER", "Woke from deep sleep: screen tap"); break;
        default: break;
    }
    lastActivityMs = millis();
}

inline void enterDeepSleep() {
    logDebug("POWER", "No activity for " + String(SLEEP_AFTER_MS / 1000) + " s, entering deep sleep "
                      "(wakes on CAN traffic or a tap)");
    logStop();   // close an SD capture cleanly
    Serial.flush();

    twai_stop();
    twai_driver_uninstall();

    tft.writecommand(0x10);                       // ILI9341 sleep in: panel draws almost nothing
    digitalWrite(TFT_BACKLIGHT_PIN, LOW);
    gpio_hold_en((gpio_num_t)TFT_BACKLIGHT_PIN);  // keep the backlight off while asleep
    gpio_deep_sleep_hold_en();

    // CAN RX idles high (recessive); the first dominant bit on the bus pulls it low.
    // The pull-up keeps it high when no transceiver is fitted (bench).
    esp_sleep_enable_ext0_wakeup((gpio_num_t)CAN_RX_PIN, 0);
    rtc_gpio_pullup_en((gpio_num_t)CAN_RX_PIN);
    rtc_gpio_pulldown_dis((gpio_num_t)CAN_RX_PIN);

    // The touch controller pulls its IRQ line low while the screen is pressed
    esp_sleep_enable_ext1_wakeup(1ULL << XPT2046_IRQ, ESP_EXT1_WAKEUP_ALL_LOW);

    esp_deep_sleep_start();
}

// Call on every loop() pass, after the CAN and touch handling
inline void powerTick() {
    unsigned long idle = millis() - lastActivityMs;
    if (SCREEN_OFF_AFTER_MS > 0 && !screenIsOff && idle > SCREEN_OFF_AFTER_MS) setScreenOn(false);
    if (SLEEP_AFTER_MS > 0 && idle > SLEEP_AFTER_MS) enterDeepSleep();
}

#endif
