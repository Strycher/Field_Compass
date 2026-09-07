// fram.cpp -- extracted from src.ino by scripts/extract_unit.py (E4).
#include "fram.h"
#include "fc_config.h"
#include "logging.h"

Adafruit_FRAM_SPI fram = Adafruit_FRAM_SPI(FRAM_CS);  // SPI FRAM 256KB
bool framAvailable = false;           // SPI FRAM 256KB
FRAMHeader framHeader;

// Read FRAM header into RAM (use sizeof to avoid overflowing the struct)
void framReadHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    buf[i] = fram.read8(FRAM_HEADER_ADDR + i);
  }
  memcpy(&framHeader, buf, sizeof(framHeader));  // only copy struct-sized bytes
}

// Write the RAM header back to FRAM (zero-pad to fill full 64-byte region)
void framWriteHeader() {
  uint8_t buf[FRAM_HEADER_SIZE];
  memset(buf, 0, FRAM_HEADER_SIZE);              // zero-fill padding bytes
  memcpy(buf, &framHeader, sizeof(framHeader));   // copy struct into buffer
  for (int i = 0; i < FRAM_HEADER_SIZE; i++) {
    fram.write8(FRAM_HEADER_ADDR + i, buf[i]);
  }
}

// Format FRAM with clean header (zeroed ring buffers)
void framFormat() {
  logPrintln("  Formatting...");
  memset(&framHeader, 0, sizeof(framHeader));
  framHeader.magic = FRAM_MAGIC;
  framHeader.version = FRAM_VERSION;
  framHeader.battCapacity = FRAM_BATT_COUNT;
  framHeader.wxCapacity = FRAM_WX_COUNT;
  framWriteHeader();
  logPrintln("  Format complete");
}

void initFRAM() {
  logPrint("Initializing FRAM... ");
  // Safety check: struct must fit within FRAM header region
  static_assert(sizeof(FRAMHeader) <= FRAM_HEADER_SIZE, "FRAMHeader exceeds FRAM_HEADER_SIZE");

  if (!fram.begin()) {
    logPrintln("NOT FOUND (check wiring)");
    return;
  }

  // Verify FRAM by reading manufacturer/product IDs
  uint8_t mfgId;
  uint16_t prodId;
  fram.getDeviceID(&mfgId, &prodId);
  framAvailable = true;
  logPrintf("OK (mfg:0x%02X prod:0x%04X, 256KB, hdr=%d/%d bytes)\n",
            mfgId, prodId, sizeof(FRAMHeader), FRAM_HEADER_SIZE);

  // Read and validate FRAM header
  framReadHeader();
  if (framHeader.magic != FRAM_MAGIC || framHeader.version != FRAM_VERSION) {
    logPrintln("  Header invalid - formatting");
    framFormat();
  } else {
    logPrintf("  Batt:%d/%d Wx:%d/%d pending\n",
              framHeader.battCount, framHeader.battCapacity,
              framHeader.wxCount, framHeader.wxCapacity);
  }
}
