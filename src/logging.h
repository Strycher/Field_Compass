#pragma once
// logging.h -- extracted from src.ino by scripts/extract_unit.py (E4).
#include <Arduino.h>
#include <SD.h>
#include <time.h>

#define LOG_LEVEL_NONE  0
#define LOG_LEVEL_ERROR 1
#define LOG_LEVEL_WARN  2
#define LOG_LEVEL_INFO  3
#define LOG_LEVEL_DEBUG 4
#define LOG_LEVEL LOG_LEVEL_INFO  // Default: ERROR + WARN + INFO
#define LOG_DIR "/logs"
#define LOG_RETENTION_HOURS   48       // Normal retention window
#define LOG_GRACE_HOURS       24       // Grace period after extended off
#define LOG_SD_BUF_SIZE       512      // RAM buffer before SD write
#define SERIAL_RING_SIZE 4096  // 4KB ring buffer for serial capture
#define SD_MAX_CONSECUTIVE_FAILURES 3
#define SD_MAX_REINIT_ATTEMPTS 10
#define SD_REINIT_COOLDOWN 15000  // Wait 15s between re-init attempts (#116)

enum SDErrorType {
  SD_ERR_NONE = 0,
  SD_ERR_OPEN_FAIL,
  SD_ERR_READ_FAIL,
  SD_ERR_WRITE_FAIL,
  SD_ERR_REINIT_FAIL
};

struct SDHealth {
  bool available;              // Current availability status
  unsigned long lastSuccess;   // millis() of last successful operation
  unsigned long lastAttempt;   // millis() of last attempted operation
  uint16_t errorCount;         // Total errors since boot
  uint8_t consecutiveFailures; // Consecutive failures (resets on success)
  uint8_t reInitCount;         // Re-initialization attempts
  uint8_t lastError;           // Last error type (SDErrorType)
  unsigned long lastReInit;    // millis() of last re-init attempt
};

extern char serialRing[SERIAL_RING_SIZE];
extern volatile uint16_t serialRingHead;
extern volatile uint16_t serialRingTail;
extern bool serialLogActive;
extern unsigned long lastLogFlush;
extern unsigned long lastLogRotation;
extern char serialLogFilename[40];
extern bool sdAvailable;
extern SDHealth sdHealth;

void serialRingAppend(const char* str);
String serialRingRead();
String serialRingPeek();
void logPrint(const char* msg);
void logPrintln(const char* msg);
void logPrintf(const char* fmt, ...);
const char* logTimestamp();
void serialLogAppend(const char* str);
void serialLogFlush();
void serialLogRotate();
void initSerialLog(bool rtcOk);   // rtcOk: the caller's RTC availability (#263)
void recordSDSuccess();
void recordSDError(SDErrorType err);
bool shouldAttemptReInit();
bool trySDReInit();
File sdOpenSafe(const char* path, const char* mode, bool silent = false);

// Severity-level log macros — compile to nothing when below LOG_LEVEL (#39)
// Usage: LOG_INFO("WiFi connected to %s", ssid);
// Output: [12:34:56] INFO  WiFi connected to <ssid>
#if LOG_LEVEL >= LOG_LEVEL_ERROR
  #define LOG_ERROR(fmt, ...) logPrintf("%sERROR " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_ERROR(fmt, ...) ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_WARN
  #define LOG_WARN(fmt, ...)  logPrintf("%sWARN  " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_WARN(fmt, ...)  ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_INFO
  #define LOG_INFO(fmt, ...)  logPrintf("%sINFO  " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_INFO(fmt, ...)  ((void)0)
#endif

#if LOG_LEVEL >= LOG_LEVEL_DEBUG
  #define LOG_DEBUG(fmt, ...) logPrintf("%sDEBUG " fmt "\n", logTimestamp(), ##__VA_ARGS__)
#else
  #define LOG_DEBUG(fmt, ...) ((void)0)
#endif

