// logging.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "logging.h"
#include "fc_config.h"
#include <SPI.h>

char serialRing[SERIAL_RING_SIZE];
volatile uint16_t serialRingHead = 0;
volatile uint16_t serialRingTail = 0;
static File serialLogFile;
bool serialLogActive = false;
static char serialLogBuf[LOG_SD_BUF_SIZE];
static uint16_t serialLogBufPos = 0;
unsigned long lastLogFlush = 0;
unsigned long lastLogRotation = 0;
char serialLogFilename[40];  // "/logs/serial_YYYYMMDD_HHMMSS.log"
bool sdAvailable = false;
SDHealth sdHealth = {false, 0, 0, 0, 0, 0, SD_ERR_NONE, 0};

void serialRingAppend(const char* str) {
  while (*str) {
    serialRing[serialRingHead] = *str++;
    serialRingHead = (serialRingHead + 1) % SERIAL_RING_SIZE;
    // If we catch up to tail, advance tail (lose oldest data)
    if (serialRingHead == serialRingTail) {
      serialRingTail = (serialRingTail + 1) % SERIAL_RING_SIZE;
    }
  }
}

// Read and clear the ring buffer
String serialRingRead() {
  String result;
  result.reserve(SERIAL_RING_SIZE);
  while (serialRingTail != serialRingHead) {
    result += serialRing[serialRingTail];
    serialRingTail = (serialRingTail + 1) % SERIAL_RING_SIZE;
  }
  return result;
}

// Peek at ring buffer without clearing
String serialRingPeek() {
  String result;
  result.reserve(SERIAL_RING_SIZE);
  uint16_t pos = serialRingTail;
  while (pos != serialRingHead) {
    result += serialRing[pos];
    pos = (pos + 1) % SERIAL_RING_SIZE;
  }
  return result;
}

// Custom print that captures to ring buffer and SD log (#59)
void logPrint(const char* msg) {
  serialRingAppend(msg);
  serialLogAppend(msg);
  Serial.print(msg);
}

void logPrintln(const char* msg) {
  serialRingAppend(msg);
  serialRingAppend("\n");
  serialLogAppend(msg);
  serialLogAppend("\n");
  Serial.println(msg);
}

void logPrintf(const char* fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  serialRingAppend(buf);
  serialLogAppend(buf);
  Serial.print(buf);
}

// Timestamp helper for LOG_* macros (#39)
// Returns "[HH:MM:SS] " (wall clock) or "[UUU:MM:SS] " (uptime if no time source)
const char* logTimestamp() {
  static char tsBuf[16];
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 0)) {
    snprintf(tsBuf, sizeof(tsBuf), "[%02d:%02d:%02d] ",
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  } else {
    unsigned long s = millis() / 1000;
    snprintf(tsBuf, sizeof(tsBuf), "[%03lu:%02lu:%02lu] ",
             s / 3600, (s % 3600) / 60, s % 60);
  }
  return tsBuf;
}

void serialLogAppend(const char* str) {
  if (!serialLogActive) return;
  while (*str) {
    serialLogBuf[serialLogBufPos++] = *str++;
    if (serialLogBufPos >= LOG_SD_BUF_SIZE) {
      serialLogFlush();
    }
  }
}

void serialLogFlush() {
  if (!serialLogActive || serialLogBufPos == 0) return;
  if (serialLogFile) {
    size_t written = serialLogFile.write((uint8_t*)serialLogBuf, serialLogBufPos);
    if (written != (size_t)serialLogBufPos) {
      // Write failure — close and reopen, retry once
      serialLogFile.close();
      serialLogFile = SD.open(serialLogFilename, FILE_APPEND);
      if (serialLogFile) {
        serialLogFile.write((uint8_t*)serialLogBuf, serialLogBufPos);
      } else {
        serialLogActive = false;
        LOG_ERROR("[LOG] SD write failed, logging disabled");
      }
    }
    serialLogFile.flush();
  }
  serialLogBufPos = 0;
  lastLogFlush = millis();
}

void serialLogRotate() {
  if (!sdAvailable) return;

  File dir = SD.open(LOG_DIR);
  if (!dir || !dir.isDirectory()) return;

  // First pass: find the most recent existing log timestamp
  time_t now;
  time(&now);
  time_t newestLog = 0;

  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      int yr, mo, dy, hr, mn, sc;
      if (sscanf(name, "serial_%4d%2d%2d_%2d%2d%2d.log", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
        struct tm t = {};
        t.tm_year = yr - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = dy;
        t.tm_hour = hr;
        t.tm_min = mn;
        t.tm_sec = sc;
        time_t logTime = mktime(&t);
        if (logTime > newestLog) newestLog = logTime;
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  // Decide retention: normal (48h) or grace (keep all)
  double hoursSinceNewest = (newestLog > 0) ? difftime(now, newestLog) / 3600.0 : 0;
  bool graceMode = (newestLog > 0 && hoursSinceNewest >= LOG_RETENTION_HOURS);

  if (graceMode) {
    logPrintf("[LOG] Grace mode: last log %.0fh old, keeping all files\n", hoursSinceNewest);
    return;  // Keep everything — rotation resumes after LOG_GRACE_HOURS of uptime
  }

  // Normal mode: delete files older than LOG_RETENTION_HOURS
  time_t cutoff = now - ((time_t)LOG_RETENTION_HOURS * 3600);
  int deleted = 0;

  dir = SD.open(LOG_DIR);
  entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      const char* name = entry.name();
      int yr, mo, dy, hr, mn, sc;
      if (sscanf(name, "serial_%4d%2d%2d_%2d%2d%2d.log", &yr, &mo, &dy, &hr, &mn, &sc) == 6) {
        struct tm t = {};
        t.tm_year = yr - 1900;
        t.tm_mon = mo - 1;
        t.tm_mday = dy;
        t.tm_hour = hr;
        t.tm_min = mn;
        t.tm_sec = sc;
        time_t logTime = mktime(&t);
        if (logTime < cutoff) {
          char fullPath[60];
          snprintf(fullPath, sizeof(fullPath), LOG_DIR "/%s", name);
          entry.close();
          SD.remove(fullPath);
          deleted++;
          entry = dir.openNextFile();
          continue;
        }
      }
    }
    entry.close();
    entry = dir.openNextFile();
  }
  dir.close();

  if (deleted > 0) {
    logPrintf("[LOG] Rotation: deleted %d files older than %dh\n", deleted, LOG_RETENTION_HOURS);
  }
}

// rtcOk is passed in rather than read from the RTC's global (#263): the RTC
// belongs to whichever unit owns it in E4-5, and this unit should not reach
// back into src.ino for it. The single caller is setup().
void initSerialLog(bool rtcOk) {
  if (!sdAvailable || !rtcOk) {
    logPrintln("[LOG] Serial log disabled (no SD or RTC)");
    return;
  }

  // Create /logs/ directory if missing
  if (!SD.exists(LOG_DIR)) {
    SD.mkdir(LOG_DIR);
  }

  // Build filename from RTC time
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    logPrintln("[LOG] Serial log disabled (no time source)");
    return;
  }

  snprintf(serialLogFilename, sizeof(serialLogFilename),
           LOG_DIR "/serial_%04d%02d%02d_%02d%02d%02d.log",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
           timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  // Run smart rotation before opening new file
  serialLogRotate();

  // Open log file for append
  serialLogFile = SD.open(serialLogFilename, FILE_APPEND);
  if (!serialLogFile) {
    logPrintf("[LOG] Failed to open %s\n", serialLogFilename);
    return;
  }

  serialLogActive = true;
  serialLogBufPos = 0;
  lastLogFlush = millis();
  lastLogRotation = millis();
  logPrintf("[LOG] Logging to %s\n", serialLogFilename);
}

// Record successful SD operation
void recordSDSuccess() {
  sdHealth.lastSuccess = millis();
  sdHealth.consecutiveFailures = 0;
}

// Record SD error and potentially trigger re-init
void recordSDError(SDErrorType err) {
  sdHealth.lastError = err;
  sdHealth.errorCount++;
  sdHealth.consecutiveFailures++;
  sdHealth.lastAttempt = millis();

  logPrintf("[SD] Error %d (total:%d consec:%d)\n",
            err, sdHealth.errorCount, sdHealth.consecutiveFailures);

  // If too many consecutive failures, try re-init
  if (sdHealth.consecutiveFailures >= SD_MAX_CONSECUTIVE_FAILURES) {
    trySDReInit();
  }
}

// Check if we should attempt SD re-initialization
bool shouldAttemptReInit() {
  // Don't exceed max attempts
  if (sdHealth.reInitCount >= SD_MAX_REINIT_ATTEMPTS) return false;

  // Enforce cooldown period
  if (millis() - sdHealth.lastReInit < SD_REINIT_COOLDOWN) return false;

  return true;
}

// Attempt SD re-initialization with backoff
bool trySDReInit() {
  if (!shouldAttemptReInit()) {
    logPrintln("[SD] Re-init skipped (cooldown or max attempts)");
    return false;
  }

  sdHealth.reInitCount++;
  sdHealth.lastReInit = millis();

  logPrintf("[SD] Attempting re-init #%d...\n", sdHealth.reInitCount);

  // Full SPI bus reset: end SD, end SPI, re-init SPI, then re-mount SD (#116)
  // This clears any stale bus state from TFT_eSPI's 80MHz DMA transfers
  SD.end();
  SPI.end();
  delay(100);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
  delay(100);  // Allow bus + card to settle

  // Try to re-initialize with conservative 4MHz clock (#116)
  if (SD.begin(SD_CS, SPI, 4000000)) {
    sdAvailable = true;
    sdHealth.available = true;
    sdHealth.consecutiveFailures = 0;
    sdHealth.lastSuccess = millis();
    logPrintln("[SD] Re-init SUCCESS");
    return true;
  } else {
    sdAvailable = false;
    sdHealth.available = false;
    sdHealth.lastError = SD_ERR_REINIT_FAIL;
    logPrintln("[SD] Re-init FAILED");
    return false;
  }
}

// Safe file open with error tracking
File sdOpenSafe(const char* path, const char* mode, bool silent) {
  if (!sdHealth.available) {
    return File();  // Return invalid file
  }

  sdHealth.lastAttempt = millis();

  // Try open with one retry on failure (bus contention mitigation #116)
  File f;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (strcmp(mode, "r") == 0 || strcmp(mode, FILE_READ) == 0) {
      f = SD.open(path, FILE_READ);
    } else if (strcmp(mode, "w") == 0 || strcmp(mode, FILE_WRITE) == 0) {
      f = SD.open(path, FILE_WRITE);
    } else if (strcmp(mode, "a") == 0) {
      f = SD.open(path, FILE_APPEND);
    } else {
      f = SD.open(path);  // Default mode
    }
    if (f) break;  // Success
    if (attempt == 0) delay(50);  // Brief settle before retry
  }

  if (!f) {
    if (!silent) {
      recordSDError(SD_ERR_OPEN_FAIL);
    }
    return File();
  }

  recordSDSuccess();
  return f;
}
