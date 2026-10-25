#ifndef CAN_SNIFFER_H
#define CAN_SNIFFER_H

#include <Arduino.h>
#include "config.h"
#include "can_logger.h"

// ---- Listen-only CAN sniffer ------------------------------------------------------
// While the sniffer page is shown the controller is in listen-only mode and every frame
// on the bus is recorded here: one row per ID with its latest data, which bytes changed
// recently and how often it is sent. Used to find values the truck broadcasts on its own
// (wheel speeds, transfer case, ...) without sending any requests.
// A summary of all IDs is also logged to serial every SNIFF_LOG_INTERVAL_MS.

struct SniffEntry {
    uint32_t      id;
    bool          extd;                 // 29-bit ID (not expected on this truck)
    uint8_t       dlc;
    uint8_t       data[8];
    unsigned long changedAt[8];         // millis() when each byte last changed (0 = never)
    uint32_t      count;                // frames seen
    uint32_t      countAtLastRate;
    uint16_t      rate;                 // frames per second over the last second
};

static SniffEntry sniffIds[MAX_SNIFF_IDS];   // kept sorted by ID
static int        sniffIdCount   = 0;
static uint32_t   sniffTableFull = 0;        // frames dropped because the table was full
static uint16_t   sniffBusRate   = 0;        // all frames per second
static int        sniffScroll    = 0;        // first row shown on the sniffer page
static uint32_t   sniffTotal = 0, sniffTotalAtLastRate = 0;

inline void sniffReset() {
    sniffIdCount = sniffScroll = 0;
    sniffTableFull = sniffTotal = sniffTotalAtLastRate = 0;
    sniffBusRate = 0;
}

// Find an ID's row, adding it in sorted position the first time it is seen
inline SniffEntry* sniffFind(uint32_t id, bool extd) {
    int pos = 0;
    while (pos < sniffIdCount && (sniffIds[pos].extd < extd ||
           (sniffIds[pos].extd == extd && sniffIds[pos].id < id))) pos++;
    if (pos < sniffIdCount && sniffIds[pos].id == id && sniffIds[pos].extd == extd) return &sniffIds[pos];
    if (sniffIdCount >= MAX_SNIFF_IDS) return nullptr;

    memmove(&sniffIds[pos + 1], &sniffIds[pos], (sniffIdCount - pos) * sizeof(SniffEntry));
    sniffIdCount++;
    SniffEntry& e = sniffIds[pos];
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.extd = extd;
    logDebug("CAN_SNIFF", "New ID " + String(id, HEX));
    return &e;
}

inline void sniffRecord(const twai_message_t& msg) {
    sniffTotal++;
    SniffEntry* e = sniffFind(msg.identifier, msg.extd);
    if (!e) { sniffTableFull++; return; }

    uint8_t dlc = msg.rtr ? 0 : min((int)msg.data_length_code, 8);
    unsigned long now = millis();
    if (e->count > 0) {
        for (int i = 0; i < 8; i++) {
            bool wasThere = i < e->dlc, isThere = i < dlc;
            if (wasThere != isThere || (isThere && e->data[i] != msg.data[i])) e->changedAt[i] = now;
        }
    }
    e->dlc = dlc;
    memcpy(e->data, msg.data, dlc);
    e->count++;
}

inline String sniffDataHex(const SniffEntry& e) {
    String s = "";
    char hex[4];
    for (int i = 0; i < e.dlc; i++) {
        snprintf(hex, sizeof(hex), "%02X ", e.data[i]);
        s += hex;
    }
    return s;
}

inline void sniffLogSummary() {
    twai_status_info_t st;
    uint32_t missed = (twai_get_status_info(&st) == ESP_OK) ? st.rx_missed_count : 0;
    logDebug("CAN_SNIFF", String(sniffIdCount) + " IDs, " + String(sniffBusRate) + " frames/s, " +
                          String(missed) + " missed (queue full), " + String(sniffTableFull) + " not stored (table full)");
    for (int i = 0; i < sniffIdCount; i++) {
        const SniffEntry& e = sniffIds[i];
        Serial.printf("  %s%03X [%d] %-24s %4u/s\n", e.extd ? "x" : "", e.id, e.dlc, sniffDataHex(e).c_str(), e.rate);
    }
}

// Read every waiting frame. Called on every loop() pass while sniffing, so the receive
// queue (128 frames) doesn't overflow on a busy bus.
inline void sniffPoll() {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        int64_t us = esp_timer_get_time();
        sniffRecord(msg);
        logFrame(msg, us);   // only writes while recording to SD
    }
    logTick();

    static unsigned long lastRate = 0, lastLog = 0;
    unsigned long now = millis();
    if (now - lastRate >= 1000) {
        float secs = (now - lastRate) / 1000.0;
        for (int i = 0; i < sniffIdCount; i++) {
            sniffIds[i].rate = (sniffIds[i].count - sniffIds[i].countAtLastRate) / secs;
            sniffIds[i].countAtLastRate = sniffIds[i].count;
        }
        sniffBusRate = (sniffTotal - sniffTotalAtLastRate) / secs;
        sniffTotalAtLastRate = sniffTotal;
        lastRate = now;
    }
    if (now - lastLog >= SNIFF_LOG_INTERVAL_MS) {
        lastLog = now;
        sniffLogSummary();
    }
}

#endif
