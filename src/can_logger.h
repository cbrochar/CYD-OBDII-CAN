#ifndef CAN_LOGGER_H
#define CAN_LOGGER_H

#include <Arduino.h>
#include <SD.h>
#include "config.h"
#include "sd_card.h"

// ---- SD card capture --------------------------------------------------------------
// REC on the sniffer page writes every received frame to /CANnnnn.LOG in candump log
// format, which SavvyCAN, python-can and cantools open directly:
//     (0000123.456789) can0 7E8#064100BE3FA81300
// MARK writes "(time) MARK n" to /CANnnnn.TXT, so you can note the moment you shift the
// transfer case etc. and find it in the log. Times are seconds since the CYD started
// (when a frame was read from the receive queue, so within a few ms of arrival).
// Frames are collected in RAM and written in large blocks, and the file is flushed every
// LOG_FLUSH_INTERVAL_MS, so pulling the power loses at most that much.

static bool     logRecording = false;
static bool     logError     = false;   // card missing, full or removed while recording
static File     logFile, logNotes;
static String   logName      = "";      // e.g. "CAN0003"
static uint32_t logFrames    = 0;
static uint32_t logMarks     = 0;
static char     logBuf[8192];
static size_t   logBufLen    = 0;
static unsigned long logLastFlush = 0;

// Write out the RAM buffer; a short write means the card is gone or full
inline bool logWriteBuffer() {
    if (logBufLen == 0) return true;
    size_t written = logFile.write((const uint8_t*)logBuf, logBufLen);
    bool ok = written == logBufLen;
    logBufLen = 0;
    return ok;
}

inline void logStop() {
    if (!logRecording) return;
    logRecording = false;
    logWriteBuffer();
    logFile.close();
    logNotes.close();
    logDebug("SD", "Stopped recording " + logName + ".LOG: " + String(logFrames) + " frames, " +
                   String(logMarks) + " marks");
}

inline void logFail(const String& why) {
    logDebug("SD_ERR", why + ". Recording stopped.");
    logRecording = false;
    logFile.close();
    logNotes.close();
    sdReady = false;
    logError = true;
}

inline String logTimestamp(int64_t us) {
    char t[24];
    snprintf(t, sizeof(t), "(%07lu.%06lu)", (unsigned long)(us / 1000000), (unsigned long)(us % 1000000));
    return String(t);
}

inline bool logStart() {
    if (logRecording) return true;
    logError = false;
    if (!sdBegin()) { logError = true; return false; }

    // Next free file number
    char name[16];
    int n = 1;
    for (; n <= 9999; n++) {
        snprintf(name, sizeof(name), "/CAN%04d.LOG", n);
        if (!SD.exists(name)) break;
    }
    logFile = SD.open(name, FILE_WRITE);
    snprintf(name, sizeof(name), "/CAN%04d.TXT", n);
    logNotes = SD.open(name, FILE_WRITE);
    if (!logFile || !logNotes) { logFail("Could not create log files"); return false; }

    snprintf(name, sizeof(name), "CAN%04d", n);
    logName = name;
    logNotes.printf("CYD-OBDII-CAN capture %s.LOG\n", name);
    logNotes.printf("Bus: 500 kbps, 11-bit, listen-only. Times are seconds since the CYD started.\n");
    logNotes.printf("%s START\n", logTimestamp(esp_timer_get_time()).c_str());
    logNotes.flush();

    logFrames = logMarks = 0;
    logBufLen = 0;
    logLastFlush = millis();
    logRecording = true;
    logDebug("SD", "Recording to " + logName + ".LOG");
    return true;
}

inline void logFrame(const twai_message_t& msg, int64_t us) {
    if (!logRecording) return;
    if (logBufLen > sizeof(logBuf) - 48 && !logWriteBuffer()) { logFail("SD write failed"); return; }

    char* p = logBuf + logBufLen;
    int len = snprintf(p, 40, "(%07lu.%06lu) can0 ", (unsigned long)(us / 1000000), (unsigned long)(us % 1000000));
    len += snprintf(p + len, 12, msg.extd ? "%08lX#" : "%03lX#", (unsigned long)msg.identifier);
    if (msg.rtr) {
        p[len++] = 'R';
    } else {
        static const char HEX_DIGITS[] = "0123456789ABCDEF";
        for (int i = 0; i < msg.data_length_code && i < 8; i++) {
            p[len++] = HEX_DIGITS[msg.data[i] >> 4];
            p[len++] = HEX_DIGITS[msg.data[i] & 0x0F];
        }
    }
    p[len++] = '\n';
    logBufLen += len;
    logFrames++;
}

inline void logMark() {
    if (!logRecording) return;
    logMarks++;
    String line = logTimestamp(esp_timer_get_time()) + " MARK " + String(logMarks);
    logNotes.println(line);
    logNotes.flush();
    logDebug("SD", line);
}

// Call on every loop() pass: flushes the log so a power cut loses little
inline void logTick() {
    if (!logRecording || millis() - logLastFlush < LOG_FLUSH_INTERVAL_MS) return;
    logLastFlush = millis();
    if (!logWriteBuffer()) { logFail("SD write failed"); return; }
    logFile.flush();
}

#endif
