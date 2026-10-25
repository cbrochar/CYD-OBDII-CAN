#ifndef CAN_SIMULATOR_H
#define CAN_SIMULATOR_H

#ifdef CAN_SELF_TEST

#include <Arduino.h>
#include "config.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

// ---- Self-test truck simulator ------------------------------------------------------
// Only built with -D CAN_SELF_TEST (pio run -e selftest). No transceiver is needed: the
// controller runs in no-ACK self-test mode with its TX signal looped back to RX on one
// pin inside the ESP32, so every frame it sends with the "self" flag is also received.
//
// Fake modules answer requests by sending real reply frames through the controller, so
// replies go through the same receive queue, filter and ISO-TP code as on the truck:
//   PCM 7E0 -> 7E8: 0100, 010B/0133/010C/0D/42, 2209D0, 221139, 221E1C, 03 (7 codes,
//                   multi-frame), unknown 22 DIDs get a 7F 22 31 negative reply,
//                   221E1C is answered after a 7F 22 78 "response pending"
//   ABS 760 -> 768: 1800FF00 (1 code), wheel speeds 222B00/01/02
// While the sniffer page is shown, 15 broadcast IDs are sent at 10-200 ms intervals.
// Every 90 s the "key" goes off for 15 s and nothing answers, to test link loss/reconnect.

// Route the TWAI TX output and RX input to the same pin. Called after
// twai_driver_install(), which configured that pin as input only.
inline void simLoopbackPin() {
    gpio_num_t pin = (gpio_num_t)CAN_TX_PIN;
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    esp_rom_gpio_connect_out_signal(pin, TWAI_TX_IDX, false, false);
    esp_rom_gpio_connect_in_signal(pin, TWAI_RX_IDX, false);
}

inline void simSend(uint32_t id, const uint8_t* data, uint8_t len) {
    twai_message_t msg = {};
    msg.identifier = id;
    msg.self = 1;   // receive our own frame
    msg.data_length_code = len;
    memcpy(msg.data, data, len);
    twai_transmit(&msg, pdMS_TO_TICKS(20));
}

// Simulated ignition: on for 75 s, off for 15 s
inline bool simKeyOn() { return (millis() / 1000) % 90 < 75; }

// Slowly varying test value: triangle wave between lo and hi
inline float simWave(float lo, float hi, unsigned long periodMs, unsigned long phaseMs = 0) {
    float t = ((millis() + phaseMs) % periodMs) / (float)periodMs;
    return lo + (hi - lo) * (t < 0.5 ? t * 2 : 2 - t * 2);
}

// Rest of a multi-frame reply, waiting for the tester's Flow Control frame
static uint8_t  simPending[64];
static int      simPendingLen = 0, simPendingPos = 0;
static uint32_t simPendingId  = 0;

// Send a reply as ISO-TP: a Single Frame if it fits, otherwise a First Frame now and
// the Consecutive Frames once Flow Control arrives (simOnFrame)
inline void simReply(uint32_t rxId, const uint8_t* p, int len) {
    if (len <= 7) {
        uint8_t f[8] = {(uint8_t)len};
        memcpy(f + 1, p, len);
        simSend(rxId, f, 8);   // padded to 8 like real modules
        return;
    }
    len = min(len, (int)sizeof(simPending));
    uint8_t f[8] = {(uint8_t)(0x10 | (len >> 8)), (uint8_t)len};
    memcpy(f + 2, p, 6);
    simSend(rxId, f, 8);
    memcpy(simPending, p, len);
    simPendingLen = len;
    simPendingPos = 6;
    simPendingId  = rxId;
}

inline void simSendConsecutive() {
    uint8_t seq = 1;
    while (simPendingPos < simPendingLen) {
        int n = min(7, simPendingLen - simPendingPos);
        uint8_t f[8] = {(uint8_t)(0x20 | seq)};
        memcpy(f + 1, simPending + simPendingPos, n);
        simSend(simPendingId, f, 8);
        simPendingPos += n;
        seq = (seq + 1) & 0x0F;
    }
    simPendingLen = 0;
}

inline void simNegative(uint32_t rxId, uint8_t sid, uint8_t code) {
    uint8_t r[3] = {0x7F, sid, code};
    simReply(rxId, r, 3);
}

inline void simPcm(const uint8_t* req, int len) {
    const uint32_t RX = 0x7E8;
    uint8_t r[16];
    if (req[0] == 0x01 && len >= 2) {
        r[0] = 0x41; r[1] = req[1];
        uint16_t v;
        switch (req[1]) {
            case 0x00: r[2] = 0xBE; r[3] = 0x3F; r[4] = 0xA8; r[5] = 0x13; simReply(RX, r, 6); return;
            case 0x0B: r[2] = (uint8_t)simWave(100, 250, 20000); simReply(RX, r, 3); return;   // MAP kPa
            case 0x33: r[2] = 99; simReply(RX, r, 3); return;                                    // baro kPa
            case 0x0C: v = simWave(700, 3000, 15000) * 4;                                         // RPM
                       r[2] = v >> 8; r[3] = v; simReply(RX, r, 4); return;
            case 0x0D: r[2] = (uint8_t)simWave(0, 110, 30000); simReply(RX, r, 3); return;     // km/h
            case 0x42: v = simWave(13600, 14400, 10000);                                         // mV
                       r[2] = v >> 8; r[3] = v; simReply(RX, r, 4); return;
            default:   return;   // unsupported Mode 01 PIDs get no reply
        }
    }
    if (req[0] == 0x22 && len >= 3) {
        uint16_t did = (req[1] << 8) | req[2], v;
        r[0] = 0x62; r[1] = req[1]; r[2] = req[2];
        switch (did) {
            case 0x09D0: v = simWave(46.5, 49, 12000) * 256; break;   // FICM V x 256
            case 0x1139: v = simWave(80, 95, 25000) * 256;   break;   // ECT degC x 256
            case 0x1E1C: simNegative(RX, 0x22, 0x78);                  // "response pending" first
                         v = simWave(150, 200, 25000) * 8;   break;   // TFT degF x 8
            default:     simNegative(RX, 0x22, 0x31); return;          // request out of range
        }
        r[3] = v >> 8; r[4] = v;
        simReply(RX, r, 5);
        return;
    }
    if (req[0] == 0x03) {
        static const uint16_t CODES[] = {0x0470, 0x2291, 0x0299, 0x0401, 0x0236, 0x2263, 0x0606};
        int n = 0;
        r[n++] = 0x43;
        r[n++] = 7;
        for (uint16_t c : CODES) { r[n++] = c >> 8; r[n++] = c; }
        simReply(RX, r, n);   // 16 bytes: First Frame + 2 Consecutive Frames
        return;
    }
    simNegative(RX, req[0], 0x11);   // service not supported
}

inline void simAbs(const uint8_t* req, int len) {
    const uint32_t RX = 0x768;
    uint8_t r[8];
    if (req[0] == 0x18) {
        r[0] = 0x58; r[1] = 1; r[2] = 0x40; r[3] = 0x35; r[4] = 0xE8;   // 1 code (C0035)
        simReply(RX, r, 5);
        return;
    }
    if (req[0] == 0x22 && len >= 3 && req[1] == 0x2B && req[2] <= 0x02) {
        // Wheel speeds in km/h x 100, each wheel slightly different
        uint16_t v = simWave(0, 110, 30000, req[2] * 300) * 100;
        r[0] = 0x62; r[1] = req[1]; r[2] = req[2]; r[3] = v >> 8; r[4] = v;
        simReply(RX, r, 5);
        return;
    }
    simNegative(RX, req[0], 0x31);
}

// Called by canSend() for every frame the firmware sends: requests (Single Frames)
// to 7E0 / 760, and Flow Control after a First Frame
inline void simOnFrame(uint32_t id, const uint8_t* f) {
    if (!simKeyOn()) return;
    uint8_t pci = f[0] >> 4;
    if (pci == 3) {
        if (simPendingLen > 0 && simPendingId == id + 8) simSendConsecutive();
        return;
    }
    int len = f[0] & 0x0F;
    if (pci != 0 || len < 1 || len > 7) return;
    if (id == 0x7E0)      simPcm(f + 1, len);
    else if (id == 0x760) simAbs(f + 1, len);
}

// Broadcast frames for the sniffer page: ID and send interval
struct SimBroadcast { uint32_t id; uint16_t periodMs; uint8_t dlc; };
static const SimBroadcast SIM_BROADCASTS[] = {
    {0x081, 10, 8}, {0x0F3, 20, 8}, {0x105, 20, 8}, {0x167, 50, 6}, {0x1A5, 50, 8},
    {0x201, 20, 8}, {0x211, 50, 8}, {0x230, 100, 5}, {0x3B3, 100, 8}, {0x420, 100, 8},
    {0x428, 200, 3}, {0x4B0, 20, 8}, {0x4C8, 200, 8}, {0x501, 100, 8}, {0x5B5, 200, 8},
};
static const int SIM_BROADCAST_COUNT = sizeof(SIM_BROADCASTS) / sizeof(SIM_BROADCASTS[0]);

// Run on every loop() pass: logs simulated key changes and, while sniffing, sends the
// broadcast frames that are due. Every third ID has a rolling counter and a moving
// value, every third a moving value only, the rest never change.
inline void simTick(bool sniffing) {
    static bool lastKey = true;
    bool key = simKeyOn();
    if (key != lastKey) {
        lastKey = key;
        logDebug("SIM", key ? "Simulated key ON: modules answering again" : "Simulated key OFF: modules silent for 15 s");
    }
    if (!sniffing || !key) return;

    static unsigned long lastSent[SIM_BROADCAST_COUNT];
    static uint8_t counter[SIM_BROADCAST_COUNT];
    unsigned long now = millis();
    for (int k = 0; k < SIM_BROADCAST_COUNT; k++) {
        const SimBroadcast& b = SIM_BROADCASTS[k];
        if (now - lastSent[k] < b.periodMs) continue;
        lastSent[k] = now;
        uint8_t d[8];
        for (int i = 0; i < 8; i++) d[i] = (uint8_t)(b.id * 7 + i * 31);   // constant bytes
        uint16_t v = simWave(0, 10000, 8000 + k * 1000);
        if (k % 3 == 0) { d[0] = counter[k]++; d[1] = v >> 8; d[2] = v; }
        else if (k % 3 == 1) { d[4 % b.dlc] = v >> 8; d[5 % b.dlc] = v; }
        if (b.id == 0x4B0) {   // four "wheel speeds" moving together
            uint16_t s = simWave(0, 11000, 30000);
            for (int w = 0; w < 4; w++) { d[w * 2] = (s + w * 5) >> 8; d[w * 2 + 1] = s + w * 5; }
        }
        simSend(b.id, d, b.dlc);
    }
}

#endif   // CAN_SELF_TEST
#endif
