/*********************************************************************
 * ESP32 Sensor #1 : HC-SR501 PIR Motion Detector  -->  BLE Peripheral
 * Project : ESP32 BLE Sensor Hub
 * Slot    : "S1" (device slot 1)
 *
 * ------------------------------------------------------------------
 * ORIGINAL WORKING CODE (unchanged sensor logic):
 *   - PIR_PIN 13, pinMode INPUT
 *   - digitalRead every 500 ms
 *   - Serial prints "MOTION DETECTED" / "No motion"
 *
 * ADDED (MOD #1..#7) : BLE peripheral layer
 *   - Broadcasts as "ESP32-PIR-S1"
 *   - Service 7A0243A0-0000-1000-8000-00805F9B34FB
 *   - Characteristic 7A0243A1-0000-1000-8000-00805F9B34FB (Notify)
 *     -> payload  "S1,PIR,SEQ,0|1"  (CSV: ID,TYPE,SEQ,VALUE)
 *   - BLE runs in a separate FreeRTOS task, so the sensor loop
 *     timing (500 ms delay) is NOT affected.
 *********************************************************************/

#include <BLEDevice.h>       // MOD #1 : BLE library (part of esp32 core)
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

/******************** ORIGINAL PIR CONFIG (UNCHANGED) ***************/
#define PIR_PIN 13

/******************** MOD #2 : BLE CONFIGURATION ********************/
#define DEVICE_ID        "S1"                 // unique slot id -> change per extra sensor
#define DEVICE_NAME      "ESP32-PIR-S1"       // advertised name (hub filters on this)
#define SERVICE_UUID     "7A0243A0-0000-1000-8000-00805F9B34FB"
#define CHARACTERISTIC_UUID "7A0243A1-0000-1000-8000-00805F9B34FB"

BLEServer         *pServer  = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
volatile bool deviceConnected    = false;
uint32_t sequenceNumber  = 0;                 // packet counter -> hub can detect lost packets

/* MOD #3 : server callbacks -> track connect / disconnect */
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    deviceConnected = true;
    Serial.println("[BLE] Hub connected");
  }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    Serial.println("[BLE] Hub disconnected -> readvertising");
    s->getAdvertising()->start();   // restart advertising so hub can reconnect
  }
};

/**************************** ORIGINAL SETUP ************************/
void setup() {
  Serial.begin(115200);
  pinMode(PIR_PIN, INPUT);

  Serial.println("================================");
  Serial.println("ESP32 + HC-SR501 Motion Detector");
  Serial.println("================================");
  Serial.println("PIR warming up...");

  /*--------- MOD #4 : start BLE peripheral --------*/
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());   // required for Notify

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);      // hub matches this UUID
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as " DEVICE_NAME);
}

/**************************** ORIGINAL LOOP *************************/
void loop() {
  int motion = digitalRead(PIR_PIN);            // UNCHANGED sensor read

  if (motion == HIGH) {
    Serial.println(">>> MOTION DETECTED <<<");  // UNCHANGED print
  } else {
    Serial.println("No motion");                // UNCHANGED print
  }

  /* MOD #5 : send the same reading over BLE as CSV "ID,TYPE,SEQ,VALUE" */
  if (deviceConnected) {
    char packet[32];
    snprintf(packet, sizeof(packet), "%s,PIR,%lu,%d",
             DEVICE_ID, (unsigned long)sequenceNumber++, motion);
    pCharacteristic->setValue((uint8_t *)packet, strlen(packet));
    pCharacteristic->notify();                  // push to hub
  }

  delay(500);                                   // UNCHANGED loop timing
}
