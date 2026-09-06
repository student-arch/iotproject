/*********************************************************************
 * ESP32 Sensor #2 : HC-SR04 Ultrasonic Distance  -->  BLE Peripheral
 * Project : ESP32 BLE Sensor Hub
 * Slot    : "S2" (device slot 2)
 *
 * Original code: Rui Santos - https://RandomNerdTutorials.com/
 *   esp32-hc-sr04-ultrasonic-arduino
 *
 * ------------------------------------------------------------------
 * ORIGINAL WORKING CODE (unchanged sensor logic):
 *   - trigPin 5, echoPin 18
 *   - 10 us trigger pulse, pulseIn() echo read
 *   - distance in cm + inch, printed every 1000 ms
 *
 * ADDED (MOD #1..#6) : BLE peripheral layer
 *   - Broadcasts as "ESP32-HCSR04-S2"
 *   - SAME service UUID as PIR node (protocol is identical),
 *     hub tells sensors apart by the DEVICE_ID field in the packet
 *     and by the advertised Bluetooth name.
 *   - Characteristic 7A0243A2-0000-1000-8000-00805F9B34FB (Notify)
 *     -> payload  "S2,HC_SR04,SEQ,centimeters"  (CSV: ID,TYPE,SEQ,VALUE)
 *   - BLE runs in a separate FreeRTOS task; sensor timing untouched.
 *********************************************************************/

#include <BLEDevice.h>       // MOD #1 : BLE library (part of esp32 core)
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

/*********** ORIGINAL HC-SR04 CONFIG (UNCHANGED PINS) ***************/
const int trigPin = 5;
const int echoPin = 18;

#define SOUND_SPEED 0.034
#define CM_TO_INCH 0.393701

long duration;
float distanceCm;
float distanceInch;

/******************** MOD #2 : BLE CONFIGURATION ********************/
#define DEVICE_ID        "S2"
#define DEVICE_NAME      "ESP32-HCSR04-S2"
#define SERVICE_UUID     "7A0243A0-0000-1000-8000-00805F9B34FB"   // same as PIR
#define CHARACTERISTIC_UUID "7A0243A2-0000-1000-8000-00805F9B34FB" // only char differs

BLEServer         *pServer  = nullptr;
BLECharacteristic *pCharacteristic = nullptr;
volatile bool deviceConnected = false;
uint32_t sequenceNumber = 0;

/* MOD #3 : server callbacks -> track connect / disconnect */
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer *s) override {
    deviceConnected = true;
    Serial.println("[BLE] Hub connected");
  }
  void onDisconnect(BLEServer *s) override {
    deviceConnected = false;
    Serial.println("[BLE] Hub disconnected -> readvertising");
    s->getAdvertising()->start();
  }
};

/**************************** ORIGINAL SETUP ************************/
void setup() {
  Serial.begin(115200);              // Starts the serial communication
  pinMode(trigPin, OUTPUT);          // Sets the trigPin as an Output
  pinMode(echoPin, INPUT);           // Sets the echoPin as an Input

  /*--------- MOD #4 : start BLE peripheral --------*/
  BLEDevice::init(DEVICE_NAME);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());

  pService->start();

  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("[BLE] Advertising as " DEVICE_NAME);
}

/**************************** ORIGINAL LOOP *************************/
void loop() {
  /* ---------------- UNCHANGED sensor code ---------------- */
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  duration = pulseIn(echoPin, HIGH);

  distanceCm   = duration * SOUND_SPEED / 2;
  distanceInch = distanceCm * CM_TO_INCH;

  Serial.print("Distance (cm): ");
  Serial.println(distanceCm);
  Serial.print("Distance (inch): ");
  Serial.println(distanceInch);
  /* -------------------------------------------------------- */

  /* MOD #5 : send distance over BLE as CSV "ID,TYPE,SEQ,VALUE(cm)" */
  if (deviceConnected) {
    char packet[32];
    snprintf(packet, sizeof(packet), "%s,HC_SR04,%lu,%.1f",
             DEVICE_ID, (unsigned long)sequenceNumber++, distanceCm);
    pCharacteristic->setValue((uint8_t *)packet, strlen(packet));
    pCharacteristic->notify();
  }

  delay(1000);                       // UNCHANGED loop timing
}
