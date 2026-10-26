#include <Arduino.h>
#include <SPI.h>
#include <SD.h>

#define SD_CS_PIN   17
#define SD_MOSI_PIN 1
#define SD_MISO_PIN 3
#define SD_SCK_PIN  2

void listDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("DIR : ");
      Serial.println(file.name());
      if (levels) {
        listDir(fs, file.path(), levels - 1);
      }
    } else {
      Serial.print("FILE: ");
      Serial.print(file.name());
      Serial.print("\tSIZE: ");
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Initializing SD card over SPI...");

  // Start SPI with your custom pins
  SPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, SPI, 4000000)) { // 4 MHz SPI speed
    Serial.println("SD card initialization failed!");
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached!");
    return;
  }

  Serial.println("SD card initialized successfully!");
  Serial.printf("Card type: %u\n", cardType);
  Serial.printf("Card size: %llu MB\n\n", SD.cardSize() / (1024ULL * 1024ULL));

  listDir(SD, "/", 1);
}

void loop() {
  // Nothing to do here
}
