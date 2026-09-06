/*********************************************************************
 * ESP32-WROOM-32 (30-pin) : CENTRAL HUB / GATEWAY
 * Project : ESP32 BLE Sensor Hub
 *
 *   ESP32-PIR-S1     ─┐
 *   ESP32-HCSR04-S2  ─┤ BLE (peripherals)
 *   future sensors   ─┘
 *                      ↓
 *                 THIS HUB (BLE central)
 *                      ↓ USB serial 115200
 *                   Laptop  (single connection)
 *
 * HOW IT WORKS
 * ------------
 * - The hub continuously scans for peripherals advertising
 *   SERVICE_UUID whose advertised name starts with "ESP32-".
 * - A match is stored (deferred-connect pattern from the official
 *   BLE_client example) and the actual connection is made from
 *   loop(), never inside the scan callback.
 * - Every sensor gets its own independent BLEClient with its own
 *   notify callback. Each client is a separate BLE connection;
 *   ESP32-WROOM supports 4 simultaneous connections
 *   (CONFIG_BT_ACL_CONNECTIONS=4).
 * - Every packet carries the sender's DEVICE_ID ("S1","S2",...), and
 *   the hub also parses the advertised name, so data from different
 *   sensors can never be mixed up.
 * - Each packet is forwarded to the single USB serial as one JSON
 *   line: {"src":"esp32hub","id":"S1","type":"PIR","seq":12,"val":"1"}
 * - Reconnection: if a sensor drops, its client callback marks the
 *   slot dead, the main loop cleans up and rescans, and the sensor
 *   (which re-advertises automatically) is picked up again.
 *
 * ADDING A NEW SENSOR LATER (no hub change needed):
 *   1. flash the new sensor with the sensor template,
 *   2. give it a name starting with "ESP32-" and a unique DEVICE_ID,
 *   3. power it on -> hub finds, connects and forwards it.
 *   (Up to 4 sensors on one ESP32-WROOM hub.)
 *********************************************************************/

#include <BLEDevice.h>
#include <BLEClient.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <ArduinoJson.h>          // install "ArduinoJson" by Benoit Blanchon (v6 or v7)

/************** CONFIG **********************************************/
#define SERVICE_UUID   "7A0243A0-0000-1000-8000-00805F9B34FB"
#define NAME_PREFIX    "ESP32-"       // whitelist: advertised name must start with this
#define SCAN_SECONDS   3              // scan window per round
#define MAX_SENSORS    4              // = CONFIG_BT_ACL_CONNECTIONS of ESP32-WROOM
#define STALE_AFTER_MS 5000           // no data for 5 s -> report sensor stale

/************** PER-SENSOR STATE ************************************/
struct SensorNode {
  BLEClient                *client = nullptr;
  BLERemoteCharacteristic  *pChar  = nullptr;
  String    name = "";
  String    id   = "?";
  String    type = "?";
  uint32_t  lastRxMs = 0;              // millis() of last received packet
  uint32_t  lastSeq  = 0;              // last sequence number (loss detection)
  bool      seqInit  = false;
  bool      alive    = false;          // connected AND subscribed
};

SensorNode nodes[MAX_SENSORS];

/* deferred connection (official BLE_client pattern) */
BLEAdvertisedDevice *pendingDevice = nullptr;
volatile bool        doConnect     = false;

uint32_t lastStatusMs = 0;

/************** FORWARD DECLARATIONS ********************************/
static void notifyCB(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify);

/************** HELPERS *********************************************/
static SensorNode *findFreeSlot() {
  for (int i = 0; i < MAX_SENSORS; i++)
    if (!nodes[i].alive) return &nodes[i];
  return nullptr;
}

static void printEvent(const char *event, const String &id, const char *extraKey = nullptr,
                       const String &extraVal = "") {
  StaticJsonDocument<192> ev;
  ev["src"]   = "esp32hub";
  ev["event"] = event;
  ev["id"]    = id;
  if (extraKey) ev[extraKey] = extraVal;
  serializeJson(ev, Serial);
  Serial.println();
}

/************** NOTIFICATION CALLBACK *******************************/
/* Runs in the BLE stack task of the client that owns this sensor.  */
static void notifyCB(BLERemoteCharacteristic *c, uint8_t *data, size_t len, bool isNotify) {
  SensorNode *n = nullptr;
  for (int i = 0; i < MAX_SENSORS; i++)
    if (nodes[i].alive && nodes[i].pChar == c) { n = &nodes[i]; break; }
  if (!n) return;

  char raw[80];
  size_t copy = (len < sizeof(raw) - 1) ? len : sizeof(raw) - 1;
  memcpy(raw, data, copy);
  raw[copy] = 0;

  /* expected packet:  "ID,TYPE,SEQ,VALUE"  e.g. "S1,PIR,42,1" */
  char *save = nullptr;
  char *idF   = strtok_r(raw, ",", &save);
  char *typeF = strtok_r(nullptr, ",", &save);
  char *seqF  = strtok_r(nullptr, ",", &save);
  char *valF  = strtok_r(nullptr, ",", &save);
  if (!idF || !typeF || !seqF || !valF) return;          // malformed -> drop

  uint32_t seq = strtoul(seqF, nullptr, 10);

  bool duplicate = n->seqInit && (seq == n->lastSeq);
  bool gap       = n->seqInit && (seq > n->lastSeq + 1);

  n->id   = idF;                                        // verified identity
  n->type = typeF;
  n->lastRxMs = millis();
  n->lastSeq  = seq;
  n->seqInit  = true;

  /* ---- one JSON line per packet -> the single USB serial ---- */
  StaticJsonDocument<192> doc;
  doc["src"]  = "esp32hub";
  doc["id"]   = n->id;
  doc["type"] = n->type;
  doc["seq"]  = seq;
  doc["val"]  = valF;
  serializeJson(doc, Serial);
  Serial.println();

  if (gap)       printEvent("gap", n->id, "seq", String(seq));
  if (duplicate) printEvent("duplicate", n->id, "seq", String(seq));
}

/************** CLIENT CALLBACKS ************************************/
class NodeClientCallbacks : public BLEClientCallbacks {
  SensorNode *node;
public:
  explicit NodeClientCallbacks(SensorNode *n) : node(n) {}
  void onDisconnect(BLEClient *c) override {
    node->alive = false;               // main loop cleans up and rescans
    printEvent("disconnected", node->id);
  }
};

/************** CONNECT TO ONE ADVERTISED SENSOR ********************/
/* Called from loop() with a heap copy of the advertised device.     */
static bool connectSensor(BLEAdvertisedDevice *dev) {
  SensorNode *n = findFreeSlot();
  if (!n) return false;

  n->name = dev->getName();
  /* provisional id from the name suffix, e.g. "ESP32-PIR-S1" -> "S1" */
  int dash = n->name.lastIndexOf('-');
  if (dash >= 0) n->id = n->name.substring(dash + 1);

  BLEClient *client = BLEDevice::createClient();
  client->setClientCallbacks(new NodeClientCallbacks(n));

  if (!client->connect(dev)) {
    printEvent("connect_failed", n->id, "name", n->name);
    return false;
  }

  BLERemoteService *svc = client->getService(SERVICE_UUID);
  if (!svc) { client->disconnect(); return false; }

  /* each sensor service has exactly one notify characteristic */
  auto *chars = svc->getCharacteristics();
  BLERemoteCharacteristic *ch = nullptr;
  if (chars && !chars->empty()) ch = chars->begin()->second;
  if (!ch || !ch->canNotify()) { client->disconnect(); return false; }

  ch->registerForNotify(notifyCB, true, true);   // enable notifications

  n->client   = client;
  n->pChar    = ch;
  n->alive    = true;
  n->lastRxMs = millis();
  n->seqInit  = false;

  printEvent("connected", n->id, "addr", dev->getAddress().toString().c_str());
  return true;
}

/************** SCAN CALLBACK ***************************************/
/* Runs in BLE task: only record + copy, never connect here.         */
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (doConnect || !dev.haveName()) return;
    String name = dev.getName();
    if (!name.startsWith(NAME_PREFIX)) return;            // whitelist
    if (!dev.isAdvertisingService(BLEUUID(SERVICE_UUID))) return;

    for (int i = 0; i < MAX_SENSORS; i++)                 // already handled?
      if (nodes[i].alive && nodes[i].name == name) return;

    BLEDevice::getScan()->stop();                         // pause scan to connect
    pendingDevice = new BLEAdvertisedDevice(dev);         // heap copy (official pattern)
    doConnect = true;
  }
};

/************** SETUP ***********************************************/
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("=== ESP32 BLE Sensor Hub ===");

  BLEDevice::init("ESP32-HUB");
  BLEDevice::setPower(ESP_PWR_LVL_P9);   // max TX power -> best range

  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks(), false, true);
  scan->setActiveScan(true);             // needed to read advertised names
}

/************** MAIN LOOP *******************************************/
void loop() {
  /* 1. deferred connect (never done inside scan callback) */
  if (doConnect && pendingDevice) {
    connectSensor(pendingDevice);
    delete pendingDevice;
    pendingDevice = nullptr;
    doConnect = false;
  }

  /* 2. housekeeping: release slots of disconnected sensors */
  for (int i = 0; i < MAX_SENSORS; i++) {
    if (nodes[i].client && !nodes[i].alive) {
      if (nodes[i].client->isConnected()) nodes[i].client->disconnect();
      nodes[i].client = nullptr;
      nodes[i].pChar  = nullptr;
    }
  }

  /* 3. watchdog: sensor connected but silent */
  uint32_t now = millis();
  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    for (int i = 0; i < MAX_SENSORS; i++) {
      if (nodes[i].alive && nodes[i].seqInit &&
          now - nodes[i].lastRxMs > STALE_AFTER_MS) {
        printEvent("stale", nodes[i].id,
                   "ms_since_packet", String(now - nodes[i].lastRxMs));
        nodes[i].lastRxMs = now;         // report once, not every second
      }
    }
  }

  /* 4. keep scanning for new / recovered sensors */
  BLEDevice::getScan()->start(SCAN_SECONDS, false);
  delay(100);
}
