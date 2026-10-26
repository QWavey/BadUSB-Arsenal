#include "BTManager.h"
#include <BLEServer.h>
#include <BLE2902.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

BLEScan* pBLEScan;
std::vector<String> foundBTDevices;
static volatile bool btScanning = false;

// Bug-hunt round 10 fix: foundBTDevices was written from the NimBLE stack
// task (MyAdvertisedDeviceCallbacks::onResult) and read/cleared from the
// main task (scanBT clears, isBTDevicePresent iterates, Ducky IF_BT_PRESENT
// calls the same). A push_back reallocation while another task walked the
// vector faulted on the freed backing store. Guard every access.
static SemaphoreHandle_t _btDevMutex = nullptr;
static void _btDevMutexEnsure() {
    if (!_btDevMutex) _btDevMutex = xSemaphoreCreateMutex();
}

BLEServer *pServer = NULL;
BLECharacteristic * pTxCharacteristic = NULL;
bool deviceConnected = false;
bool oldDeviceConnected = false;

#define SERVICE_UUID           "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    };

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};

class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String rxValue = pCharacteristic->getValue().c_str();
      if (rxValue.length() > 0) {
        Serial.println("*********");
        Serial.print("Received Value: ");
        for (int i = 0; i < rxValue.length(); i++) {
          Serial.print(rxValue[i]);
        }
        Serial.println();
        Serial.println("*********");
      }
    }
};

class MyAdvertisedDeviceCallbacks: public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        String name = advertisedDevice.getName().c_str();
        if (name.length() > 0) {
            _btDevMutexEnsure();
            if (_btDevMutex && xSemaphoreTake(_btDevMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                foundBTDevices.push_back(name);
                xSemaphoreGive(_btDevMutex);
            }
        }
    }
};

void setupBT() {
    // Watch port: null-guard the createServer call. If BLE init failed
    // (e.g. WiFi coex refused, heap fragmented, controller busy), then
    // pServer stays NULL and `setCallbacks` on it panics with
    // StoreProhibited — confirmed via serial log during the boot loop.
    BLEDevice::init(bluetoothName.c_str());

    pServer = BLEDevice::createServer();
    if (!pServer) {
        Serial.println("[BT] createServer() returned NULL — aborting setupBT");
        BLEDevice::deinit();
        return;
    }
    pServer->setCallbacks(new MyServerCallbacks());

    // Create the BLE Service
    BLEService *pService = pServer->createService(SERVICE_UUID);
    if (!pService) {
        Serial.println("[BT] createService() failed — aborting setupBT");
        BLEDevice::deinit();
        pServer = nullptr;
        return;
    }

    // Create a BLE Characteristic for TX
    pTxCharacteristic = pService->createCharacteristic(
                    CHARACTERISTIC_UUID_TX,
                    BLECharacteristic::PROPERTY_NOTIFY
                  );
                      
    pTxCharacteristic->addDescriptor(new BLE2902());

    // Create a BLE Characteristic for RX
    BLECharacteristic * pRxCharacteristic = pService->createCharacteristic(
                       CHARACTERISTIC_UUID_RX,
                      BLECharacteristic::PROPERTY_WRITE
                    );

    pRxCharacteristic->setCallbacks(new MyCallbacks());

    // Start the service
    pService->start();

    // Start advertising
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
}

void loopBT() {
    // Watch port BT freeze fix (user report: "enabling bluetooth froze
    // ESP + web became unresponsive"):
    //   1. Was calling pServer->startAdvertising() without a null guard.
    //      stopBT() deinits BLE but pServer stays non-null, so next
    //      disconnect edge crashes → hard freeze until wdt.
    //   2. delay(500) on the disconnect edge blocked the main loop for
    //      500 ms every disconnect — server.handleClient, watchUiTick,
    //      HID all stalled — visible as "web unresponsive".
    // Now: null-guard pServer; replace the delay with a millis()-based
    // 500 ms re-advertise timer so the loop stays fluid.
    if (!pServer) return;
    static unsigned long s_reAdvAt = 0;
    if (!deviceConnected && oldDeviceConnected) {
        s_reAdvAt = millis() + 500;
        oldDeviceConnected = deviceConnected;
    }
    if (s_reAdvAt && millis() >= s_reAdvAt) {
        s_reAdvAt = 0;
        if (pServer) {
            pServer->startAdvertising();
            Serial.println("Start advertising");
        }
    }
    if (deviceConnected && !oldDeviceConnected) {
        oldDeviceConnected = deviceConnected;
    }
}

void stopBT() {
    // Watch port BT freeze fix: BLEDevice::deinit() frees the underlying
    // NimBLE objects but the raw pointers stay dangling. Next loopBT() /
    // scanBT() / startBTAdvertising() call derefs freed memory → crash.
    // NULL every cached pointer here.
    BLEDevice::deinit();
    pServer            = nullptr;
    pTxCharacteristic  = nullptr;
    pBLEScan           = nullptr;
    deviceConnected    = false;
    oldDeviceConnected = false;
    btScanning         = false;
}

void scanBT() {
    // v4.4: null-guard. pBLEScan is only set after setupBT() runs; callers
    // (Ducky IF_BT_PRESENT, processAutomation with bluetoothToggleEnabled=false)
    // could reach scanBT before setupBT and crash on the null deref.
    if (!bluetoothToggleEnabled || !pBLEScan) {
        Serial.println("[BT] scanBT: BT not initialised — skipped");
        return;
    }
    // Watch port: was pBLEScan->start(5, false) — synchronous 5s block.
    // Cut to 2 s so the main loop isn't frozen for a full 5 s cycle when
    // BT discovery is on. Still plenty to catch nearby advertising devices.
    _btDevMutexEnsure();
    if (_btDevMutex && xSemaphoreTake(_btDevMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        foundBTDevices.clear();
        xSemaphoreGive(_btDevMutex);
    }
    Serial.println("Scanning for BT devices...");
    btScanning = true;
    BLEScanResults* foundDevices = pBLEScan->start(2, false);
    btScanning = false;
    if (foundDevices) {
        Serial.print("BT Devices found: ");
        Serial.println(foundDevices->getCount());
    }
    pBLEScan->clearResults();
}

bool btScanInProgress() {
    return btScanning;
}

bool isBTDevicePresent(String name) {
    // Bug-hunt round 10 fix: was walking foundBTDevices without the mutex
    // while the BLE task could push_back mid-iteration -> heap corruption
    // when the vector reallocated its backing store. Snapshot under lock.
    _btDevMutexEnsure();
    std::vector<String> snap;
    if (_btDevMutex && xSemaphoreTake(_btDevMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        snap = foundBTDevices;
        xSemaphoreGive(_btDevMutex);
    }
    for (const String& device : snap) {
        if (device.indexOf(name) != -1) return true;
    }
    return false;
}

int getBTClientCount() {
    return deviceConnected ? 1 : 0;
}

void stopBTAdvertising() {
    if (pServer) {
        BLEDevice::getAdvertising()->stop();
        Serial.println("[BT] Advertising stopped");
    }
}

void startBTAdvertising() {
    if (pServer) {
        BLEDevice::getAdvertising()->start();
        Serial.println("[BT] Advertising started");
    }
}
