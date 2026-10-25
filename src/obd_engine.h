#ifndef OBD_ENGINE_H
#define OBD_ENGINE_H

#include <Arduino.h>
#include "config.h"
#include "can_simulator.h"   // empty unless built with -D CAN_SELF_TEST

// Parse two raw response hex string bytes into an unsigned int
inline uint16_t parseHexBytes(String rawResponse, int startCharIndex) {
    if(rawResponse.length() < startCharIndex + 4) return 0;
    String byteHex = rawResponse.substring(startCharIndex, startCharIndex + 4);
    return (uint16_t) strtol(byteHex.c_str(), NULL, 16);
}

// ---- CAN transport (ESP32 TWAI + ISO 15765-2 / ISO-TP) -----------------------
// Truck is ISO 15765-4 CAN, 11-bit, 500 kbps (confirmed by the obd_logger capture).
// Each request goes to a module's request ID and the reply comes back from ID + 8
// (7E0 -> 7E8, 760 -> 768). Replies are returned as hex text with no spaces, the same
// format the ELM327 gave (e.g. "410C0AD6"), so the decoders below didn't change.

// true while the controller is in listen-only mode (sniffer page): it sends nothing,
// not even ACK bits, so it can't disturb the truck's bus
static bool canListenOnly = false;

// Start the TWAI driver. Normal mode only accepts IDs 700-7FF (diagnostic replies), so
// the truck's broadcast traffic doesn't fill the receive queue; listen-only accepts all.
inline bool canBegin(bool listenOnly = false) {
#ifdef CAN_SELF_TEST
    // Self-test: no-ACK mode, TX looped back to RX on one pin, and the sniffer can't be
    // truly listen-only since the fake broadcasts come from this controller
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_TX_PIN, TWAI_MODE_NO_ACK);
#else
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)CAN_TX_PIN, (gpio_num_t)CAN_RX_PIN,
                                                          listenOnly ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
#endif
    g.rx_queue_len = listenOnly ? 128 : 32;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (!listenOnly) {
        f.acceptance_code = (uint32_t)0x700 << 21;
        f.acceptance_mask = ~((uint32_t)0x700 << 21);   // only the top 3 ID bits must match
    }
    canListenOnly = listenOnly;
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
#ifdef CAN_SELF_TEST
    simLoopbackPin();
#endif
    return twai_start() == ESP_OK;
}

// Switch between normal (polling) and listen-only (sniffer) mode by reinstalling the driver
inline void canSetListenOnly(bool on) {
    if (on == canListenOnly || canDriverFailed) return;
    logDebug("CAN_INIT", on ? "Switching to LISTEN-ONLY mode (nothing is sent)" : "Switching to normal mode");
    twai_stop();
    twai_driver_uninstall();
    if (!canBegin(on)) {
        logDebug("CAN_FATAL", "TWAI driver failed to restart!");
        canDriverFailed = true;
    }
}

// Restart the controller after a failed request. Aborts a frame nobody ACKed
// (nothing else on the bus) and recovers from bus-off.
inline void canReset() {
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return;
    if (st.state == TWAI_STATE_BUS_OFF) {
        logDebug("CAN_WARN", "Bus-off, recovering...");
        twai_initiate_recovery();
        unsigned long start = millis();
        while (millis() - start < 500) {
            if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_STOPPED) break;
            delay(10);
        }
        twai_start();
    } else if (st.state == TWAI_STATE_RUNNING && st.tx_error_counter >= 128) {
        logDebug("CAN_WARN", "No ACK on the bus (TEC " + String(st.tx_error_counter) + "), restarting controller");
        twai_stop();
        twai_start();
    }
}

inline bool canSend(uint32_t id, const uint8_t* data, uint8_t len) {
    twai_message_t msg = {};
    msg.identifier = id;
    msg.data_length_code = 8;
    for (int i = 0; i < 8; i++) msg.data[i] = i < len ? data[i] : 0x00;   // pad to 8 bytes
    bool ok = twai_transmit(&msg, pdMS_TO_TICKS(20)) == ESP_OK;
#ifdef CAN_SELF_TEST
    if (ok) simOnFrame(id, msg.data);   // the fake modules answer through the controller
#endif
    return ok;
}

// Wait for the next frame from rxId; frames from other IDs are skipped
inline bool canReceive(uint32_t rxId, twai_message_t& msg, unsigned long deadline) {
    while ((long)(deadline - millis()) > 0) {
        if (twai_receive(&msg, pdMS_TO_TICKS(deadline - millis())) != ESP_OK) return false;
        if (msg.identifier == rxId && !msg.extd && !msg.rtr && msg.data_length_code >= 1) return true;
    }
    return false;
}

// Send one request (hex text, e.g. "010C" or "2209D0") and return the reply payload as
// hex text, or "NODATA". Multi-frame replies (First Frame + Consecutive Frames) are
// joined, so "03" with many codes comes back as one string.
inline String canRequest(uint32_t txId, const char* cmd, unsigned long timeoutMs = CAN_REPLY_TIMEOUT_MS) {
    uint8_t sf[8] = {0};   // Single Frame: PCI byte = length, then the request bytes
    uint8_t reqLen = 0;
    for (const char* p = cmd; p[0] && p[1] && reqLen < 7; p += 2) {
        char hex[3] = {p[0], p[1], 0};
        sf[1 + reqLen++] = (uint8_t)strtol(hex, NULL, 16);
    }
    sf[0] = reqLen;

    twai_clear_receive_queue();
    if (!canSend(txId, sf, reqLen + 1)) {
        canReset();
        return "NODATA";
    }

    uint32_t rxId = txId + 8;
    uint8_t payload[256];
    int total = -1, have = 0;
    uint8_t nextSeq = 1;
    unsigned long deadline = millis() + timeoutMs;
    twai_message_t msg;

    while (canReceive(rxId, msg, deadline)) {
        uint8_t pci = msg.data[0] >> 4;
        if (pci == 0 && total < 0) {                       // Single Frame
            int len = msg.data[0] & 0x0F;
            if (len < 1 || len > 7) continue;
            // 7F xx 78 = "response pending": the module needs more time, keep waiting
            if (len >= 3 && msg.data[1] == 0x7F && msg.data[3] == 0x78) {
                deadline = millis() + 1000;
                continue;
            }
            memcpy(payload, msg.data + 1, len);
            total = have = len;
        } else if (pci == 1 && total < 0) {                // First Frame
            total = min(((msg.data[0] & 0x0F) << 8) | msg.data[1], (int)sizeof(payload));
            have = min(6, total);
            memcpy(payload, msg.data + 2, have);
            uint8_t fc[3] = {0x30, 0x00, 0x00};            // Flow Control: send the rest, no gap
            canSend(txId, fc, 3);
            deadline = millis() + timeoutMs;
        } else if (pci == 2 && total > 0) {                // Consecutive Frame
            if ((msg.data[0] & 0x0F) != nextSeq) {
                logDebug("CAN_WARN", String(cmd) + " -> consecutive frame out of order");
                return "NODATA";
            }
            nextSeq = (nextSeq + 1) & 0x0F;
            int n = min(7, total - have);
            memcpy(payload + have, msg.data + 1, n);
            have += n;
            deadline = millis() + timeoutMs;
        }
        if (total >= 0 && have >= total) break;
    }

    if (total < 0 || have < total) {
        canReset();
        return "NODATA";
    }
    String resp = "";
    char hex[3];
    for (int i = 0; i < total; i++) {
        snprintf(hex, sizeof(hex), "%02X", payload[i]);
        resp += hex;
    }
    return resp;
}

// Turn a module header ("7E0", "760") into its 11-bit request ID; 0 if it isn't one
inline uint32_t headerToId(const char* header) {
    uint32_t id = strtoul(header, NULL, 16);
    return id <= 0x7FF ? id : 0;
}

// PCM requests in a row without a reply; loop() drops to demo mode at CAN_MAX_MISSED_REPLIES
static int missedPcmReplies = 0;

// Single-frame query. Returns the reply for this service (e.g. "41..." for 01,
// "62..." for 22), or "NODATA" so callers' prefix checks fail.
inline String queryCustomPID(const char* header, const char* pid) {
    if (!isConnected) return "";

    uint32_t id = headerToId(header);
    if (id == 0) return "NODATA";   // not an 11-bit CAN address (e.g. the transfer case placeholder)
    String resp = canRequest(id, pid);
    if (id == 0x7E0) missedPcmReplies = (resp == "NODATA") ? missedPcmReplies + 1 : 0;

    char replyService[3] = {(char)(pid[0] + 4), pid[1], 0};   // 01 -> 41, 22 -> 62
    if (resp.startsWith(replyService)) {
        logDebug("OBD_BUS", String(header) + " " + pid + " -> " + resp);
    } else {
        logDebug("OBD_WARN", String(header) + " " + pid + " -> no valid reply: " + resp);
    }
    return resp;
}

// Probe the PCM with "0100" (supported PIDs 01-20), which every OBD-II ECU answers.
// Called from loop() every CAN_RETRY_INTERVAL_MS while not connected.
inline void tryCanConnect() {
    String resp = canRequest(0x7E0, "0100");
    if (resp.startsWith("4100")) {
        logDebug("CAN_CON", "PCM answered 0100 -> " + resp + ". Link active.");
        missedPcmReplies = 0;
        isConnected = true;   // loop() notices the change and updates the UI
    } else {
        logDebug("CAN_WARN", "No reply from PCM (7E0). Continuing in demo mode.");
    }
}

// Turn a raw 2-byte DTC into its text form, e.g. 0x0470 -> "P0470", 0x4123 -> "C0123"
inline String decodeDTC(uint16_t raw) {
    static const char SYSTEMS[] = {'P', 'C', 'B', 'U'};
    char buf[6];
    snprintf(buf, sizeof(buf), "%c%X%03X", SYSTEMS[raw >> 14], (raw >> 12) & 0x3, raw & 0xFFF);
    return String(buf);
}

// Send a request that may get a multi-frame CAN reply and return the joined payload,
// or "" if the reply doesn't start with replyPrefix.
inline String queryMultiFrame(const char* header, const char* cmd, const char* replyPrefix) {
    uint32_t id = headerToId(header);
    String payload = id ? canRequest(id, cmd, 300) : "NODATA";
    if (!payload.startsWith(replyPrefix)) {
        logDebug("OBD_WARN", "No valid reply to " + String(cmd) + ". Raw: " + payload);
        return "";
    }
    return payload;
}

// Scan for Stored Diagnostic Trouble Codes (Mode 03) and fill dtcCodes / dtcTotal.
// On CAN the reply is "43", a count byte, then 2 bytes per code, e.g. "430204702291".
inline void checkStoredDTCs() {
    if (!isConnected) return;

    logDebug("OBD_DTC", "Scanning Mode 03 diagnostic trouble registers...");
    String payload = queryMultiFrame("7E0", "03", "43");   // engine PCM only (replies from 7E8)

    int found = 0;
    bool gotReply = payload.length() >= 4;
    if (gotReply) {
        int count = strtol(payload.substring(2, 4).c_str(), NULL, 16);
        for (int i = 4; i + 4 <= (int)payload.length() && found < count; i += 4) {
            uint16_t raw = parseHexBytes(payload, i);
            if (raw == 0) continue;
            if (found < MAX_DTCS) dtcCodes[found] = decodeDTC(raw);
            found++;
        }
    }

    if (!gotReply) {
        // Keep the last good list rather than claiming the codes cleared
        return;
    }

    dtcTotal = found;
    hasDTCWarning = found > 0;
    if (found > 0) {
        logDebug("OBD_ALERT", String(found) + " stored Diagnostic Trouble Code(s) identified! Payload: " + payload);
    } else {
        logDebug("OBD_DTC", "No trouble codes found on module.");
    }
}

// ---- ABS module ------------------------------------------------------------
// Ford HS-CAN ABS module: requests to 760, replies from 768.
// Typical Ford ABS address, not yet confirmed on this truck.
// NOTE: the wheel-speed PIDs and scale below are UNVERIFIED placeholders.
// Confirm the exact PIDs/scaling for your truck's ABS module (e.g. in FORScan)
// before trusting these numbers. Cards just show "---" if the module doesn't answer.
const char* const ABS_HEADER           = "760";
const char* const ABS_PID_LF           = "222B00";
const char* const ABS_PID_RF           = "222B01";
const char* const ABS_PID_REAR         = "222B02";   // single rear-axle sensor in the differential
const float       ABS_WHEEL_KPH_PER_BIT = 0.01;

// Count the ABS module's stored codes for the warning card (fills absDtcTotal).
// Ford modules of this era read DTCs with KWP service 18 ("1800FF00" = all codes);
// the reply is "58", a count byte, then 3 bytes per code (2 code + 1 status).
// UNVERIFIED on this truck: if the module doesn't answer, the card shows "NO DATA".
inline void checkAbsDTCs() {
    String payload = queryMultiFrame(ABS_HEADER, "1800FF00", "58");
    if (payload.length() < 4) return;   // keep the last known state
    absDtcTotal = strtol(payload.substring(2, 4).c_str(), NULL, 16);
    if (absDtcTotal > 0) {
        logDebug("OBD_ALERT", String(absDtcTotal) + " ABS trouble code(s) stored! Payload: " + payload);
    }
}

// Query one ABS wheel-speed PID; updates the value only on a valid "62xxxx" reply
inline void readWheelSpeed(const char* pid, String& out) {
    String raw = queryCustomPID(ABS_HEADER, pid);
    String expected = "6" + String(pid + 1);   // 22xxxx -> 62xxxx
    if (raw.startsWith(expected) && raw.length() >= 10) {
        float kph = parseHexBytes(raw, 6) * ABS_WHEEL_KPH_PER_BIT;
        out = String((int)(kph * 0.621371));
    }
}

inline void pollWheelLF()   { readWheelSpeed(ABS_PID_LF,   valWheelLF); }
inline void pollWheelRF()   { readWheelSpeed(ABS_PID_RF,   valWheelRF); }
inline void pollWheelRear() { readWheelSpeed(ABS_PID_REAR, valWheelRear); }

// ---- Engine / chassis values (one function per card) ------------------------

// Transfer Case Status (4x4 Control Module)
// "418F02" is a leftover non-CAN header, so this always reads NO DATA until the
// module's real 11-bit address is found (FORScan; it may be on MS-CAN instead).
inline void pollTCase() {
    String raw4x4 = queryCustomPID("418F02", "2211B2");
    if (raw4x4.startsWith("6211B2") && raw4x4.length() >= 8) {
        char state = raw4x4.charAt(6);
        if (state == '1') valTCase = "2H";
        else if (state == '2') valTCase = "4H";
        else if (state == '4') valTCase = "4L";
    }
}

// Turbo Boost = MAP - BARO (standard PIDs 0B and 33: A kPa, confirmed on this truck)
// Baro barely changes, so it is read once and cached. PID 0B tops out at 255 kPa,
// so the display caps at roughly 22 psi of boost.
inline void pollBoost() {
    static float baroKPa = 0.0;
    if (baroKPa <= 0.0) {
        String rawBARO = queryCustomPID("7E0", "0133");
        if (rawBARO.startsWith("4133") && rawBARO.length() >= 6) {
            baroKPa = strtol(rawBARO.substring(4, 6).c_str(), NULL, 16);
        }
    }
    String rawMAP = queryCustomPID("7E0", "010B");
    if (rawMAP.startsWith("410B") && rawMAP.length() >= 6) {
        float mapKPa = strtol(rawMAP.substring(4, 6).c_str(), NULL, 16);
        float boost = (mapKPa - (baroKPa > 0.0 ? baroKPa : 101.3)) * 0.145038;
        valBoost = String(boost < 0 ? 0.0 : boost, 1);
    }
}

// FICM Main Power (Powerstroke injector driver supply, ~48 V nominal)
// Raw value is volts x 256: e.g. 0x2F80 = 12160 -> 47.5 V (seen in the truck log).
inline void pollFICM() {
    String rawFICM = queryCustomPID("7E0", "2209D0");
    if (rawFICM.startsWith("6209D0") && rawFICM.length() >= 10) {
        float volts = (float)parseHexBytes(rawFICM, 6) / 256.0;
        valFICM = String(volts, 1);
        if (volts < 45.0) {
            logDebug("SYS_ALERT", "CRITICAL - FICM MAIN POWER DIPPED DANGEROUSLY TO: " + valFICM);
        }
    }
}

// Engine Coolant Temperature (ECT)
inline void pollECT() {
    String rawECT = queryCustomPID("7E0", "221139");
    if (rawECT.startsWith("621139") && rawECT.length() >= 10) {
        float ectC = (int16_t)parseHexBytes(rawECT, 6) / 256.0;
        valECT = String((ectC * 1.8) + 32, 0);
    }
}

// Transmission Fluid Temperature (TFT)
inline void pollTFT() {
    String rawTFT = queryCustomPID("7E0", "221E1C");
    if (rawTFT.startsWith("621E1C") && rawTFT.length() >= 10) {
         float tftF = ((int16_t)parseHexBytes(rawTFT, 6) / 8.0);
         valTFT = String(tftF, 0);
    }
}

// System Volts (standard Mode 01 PID 42, module voltage: ((A*256)+B)/1000)
inline void pollVolts() {
    String rawVolts = queryCustomPID("7E0", "0142");
    if (rawVolts.startsWith("4142") && rawVolts.length() >= 8) {
        valVolts = String(parseHexBytes(rawVolts, 4) / 1000.0, 1);
    }
}

// Engine RPM (standard Mode 01 PID 0C: ((A*256)+B)/4)
inline void pollRPM() {
    String rawRPM = queryCustomPID("7E0", "010C");
    if (rawRPM.startsWith("410C") && rawRPM.length() >= 8) {
        valRPM = String((int)(parseHexBytes(rawRPM, 4) / 4));
    }
}

// Vehicle Speed (standard Mode 01 PID 0D: A km/h)
inline void pollSpeed() {
    String rawSpeed = queryCustomPID("7E0", "010D");
    if (rawSpeed.startsWith("410D") && rawSpeed.length() >= 6) {
        long kph = strtol(rawSpeed.substring(4, 6).c_str(), NULL, 16);
        valSpeed = String((int)(kph * 0.621371));
    }
}

// Reads only the values on screen (the current page's cards, or the one full-screen
// gauge) using each metric's poll function. Defined in display_ui.h with the metric table.
inline void pollVisibleMetrics();

// Main polling routine (only called while connected). Only the values being shown
// are read, so each refresh is a handful of requests instead of all of them.
inline void updateVehicleData() {
    if (!isConnected) return;

    unsigned long start = millis();
    pollVisibleMetrics();
    if (currentPage == PAGE_ABS && fullScreenMetric < 0) checkAbsDTCs();   // ABS status card

    // ABS speeds are blanked off their page so stale ones are never shown when you
    // swipe back; the ABS code count is kept, since a stored fault doesn't go stale.
    if (currentPage != PAGE_ABS) valWheelLF = valWheelRF = valWheelRear = "---";

    // Engine codes: every refresh on the DTC page, otherwise every DTC_SCAN_INTERVAL_MS
    // (the status bar warning still needs them)
    static unsigned long lastDtcScan = 0;
    if (currentPage == PAGE_DTC || lastDtcScan == 0 || millis() - lastDtcScan > DTC_SCAN_INTERVAL_MS) {
        lastDtcScan = millis();
        checkStoredDTCs();
    }

    logDebug("SYS_POLL", (fullScreenMetric >= 0 ? "Full-screen gauge " + String(fullScreenMetric)
                                                 : "Page " + String(currentPage))
                         + " refreshed in " + String(millis() - start) + " ms");
}

#endif
