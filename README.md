# ESP32 BLE Sensor Hub — Project Guide

Multiple ESP32 sensor boards → **BLE** → one ESP32 Hub/Gateway → **USB/Serial** → Laptop.
One laptop connection for the whole ESP32 network.

---

## 1. Architecture

```
ESP32 #1  PIR      (S1, "ESP32-PIR-S1")     ─┐
ESP32 #2  HC-SR04  (S2, "ESP32-HCSR04-S2")  ─┤  BLE peripherals (notify)
ESP32 #3  future   ("ESP32-XXX-S3")          ─┤
ESP32 #4  future   ("ESP32-XXX-S4")          ─┘
                                              ↓ BLE central
                                       ESP32-WROOM-32 HUB ("ESP32-HUB")
                                              ↓ USB 115200 (single connection)
                                          Laptop  hub_receiver.py
```

- Hub = **BLE central**, sensors = **BLE peripherals** (server role).
- ESP32-WROOM-32 supports **4 simultaneous BLE connections**
  (`CONFIG_BT_ACL_CONNECTIONS=4` in the ESP32 Arduino core) → up to 4 sensor
  boards per hub without any hub code change.

## 2. Files

| File | Board | Purpose |
|---|---|---|
| `01_pir_sensor/01_pir_sensor.ino` | Sensor #1 | HC-SR501 on pin 13 + BLE notify |
| `02_hcsr04_sensor/02_hcsr04_sensor.ino` | Sensor #2 | HC-SR04 trig 5 / echo 18 + BLE notify |
| `03_hub_gateway/03_hub_gateway.ino` | Hub | BLE central → JSON over USB |
| `04_laptop_receiver/hub_receiver.py` | Laptop | Pretty-print / log the JSON stream |

## 3. Sensor logic — what changed and why

**Nothing in the working sensor code was removed or rewritten.** All
modifications are additive and marked `MOD #1 … #6` in the sketches:

| MOD | Change | Why |
|---|---|---|
| 1 | `#include <BLEDevice.h>` etc. | BLE library ships with the ESP32 core |
| 2 | `DEVICE_ID` / `DEVICE_NAME` / UUID defines | unique identity + service discovery |
| 3 | `ServerCallbacks` class | detect hub connect/disconnect |
| 4 | BLE init/server/service/advertise in `setup()` | let hub find the sensor |
| 5 | `snprintf` + `notify()` in `loop()` | push the same reading to the hub |
| 6 | (HC-SR04 only) nothing | pins/timing untouched |

PIR keeps `digitalRead(13)` + 500 ms cycle; HC-SR04 keeps trig/echo pins,
`pulseIn()` and 1000 ms cycle. BLE runs in the ESP32's separate BLE task, so
`delay()` and `pulseIn()` in `loop()` are unaffected.

## 4. Wiring

**PIR board (existing wiring unchanged):**
- HC-SR501 VCC → 5V, GND → GND, OUT → GPIO 13

**HC-SR04 board (existing wiring unchanged):**
- VCC → 5V (or VIN), GND → GND, Trig → GPIO 5, Echo → GPIO 18
  *(keep whatever level-shifting/resistor you already used while testing)*

**Hub board:** only USB to the laptop. No sensor wiring at all.

**Power:** all three boards can run from their own USB (safest while testing).

## 5. Data packet format (sensor → hub)

Plain CSV, small enough for one BLE notification (20-byte default MTU safe):

```
ID,TYPE,SEQ,VALUE
S1,PIR,42,1        <- motion
S2,HC_SR04,133,57.2  <- 57.2 cm
```

| Field | Meaning |
|---|---|
| `ID` | unique device slot id (`S1`, `S2`, …) — the device identity |
| `TYPE` | sensor type string |
| `SEQ` | packet counter, wraps at 2³² — lets the hub detect gaps/duplicates |
| `VALUE` | `0/1` for PIR, distance in cm for HC-SR04 |

## 6. Data format (hub → laptop)

One JSON object per line (NDJSON) on the single USB serial @115200:

```json
{"src":"esp32hub","id":"S1","type":"PIR","seq":42,"val":"1"}
{"src":"esp32hub","id":"S2","type":"HC_SR04","seq":133,"val":"57.2"}
{"src":"esp32hub","event":"connected","id":"S1","addr":"a4:cf:12:34:56:78"}
{"src":"esp32hub","event":"disconnected","id":"S1"}
{"src":"esp32hub","event":"stale","id":"S2","ms_since_packet":5230}
{"src":"esp32hub","event":"gap","id":"S2","seq":140}
```

Events tell you connect/disconnect/packet-loss (`gap`, `duplicate`, `stale`).

## 7. How the hub keeps devices separate

1. **Whitelist scan** — only devices named `ESP32-*` advertising
   `SERVICE_UUID 7A0243A0-…` are touched; other BLE devices are ignored.
2. **One BLEClient per sensor** — every sensor gets its own connection object
   with its own callback (`NodeClientCallbacks`) and its own notify handler.
3. **Identity double-check** — the advertised name suffix (e.g. `S1`) is set as
   provisional ID at connect time, and every packet's first CSV field must
   carry the same ID scheme; the hub re-reads `ID` from every packet, so
   packets are always tagged with their true origin.
4. **Per-node sequence numbers** — `gap`/`duplicate` events expose loss.
5. The notify callback matches the characteristic pointer to the slot —
   data physically cannot land in the wrong slot.

## 8. Reconnection handling

- **Sensor side:** `onDisconnect()` → `deviceConnected=false` → advertising
  restarts immediately (server callback). Sensor logic keeps running.
- **Hub side:** client `onDisconnect()` marks the slot dead → main loop
  deletes the client → scan restarts → sensor is re-found and re-subscribed
  automatically. `connected`/`disconnected` JSON events are emitted.
- If a sensor is merely out of range (no disconnect event), the hub's
  watchdog reports `stale` after 5 s of silence.

## 9. Setup & testing procedure

1. **Arduino IDE** → Boards Manager: `esp32 by Espressif` (this was tested
   against core 3.3.11). Library Manager: install **ArduinoJson** (hub only).
2. Flash `01_pir_sensor.ino` → PIR board. Open its Serial Monitor (115200):
   you should see the original PIR prints **plus** `[BLE] Advertising as ESP32-PIR-S1`.
3. Flash `02_hcsr04_sensor.ino` → HC-SR04 board. Same check with its prints.
4. Install **ArduinoJson**, then flash `03_hub_gateway.ino` → hub board.
5. Power the hub alone first. Serial Monitor shows:
   `=== ESP32 BLE Sensor Hub ===`, then (when sensors are on)
   `{"src":"esp32hub","event":"connected","id":"S1",…}` and data lines.
6. Laptop: `pip install pyserial` then
   `python3 04_laptop_receiver/hub_receiver.py --log hub.jsonl`
7. Wave at the PIR / move an object in front of the HC-SR04 and verify
   values change in the laptop stream.

## 10. Adding a 3rd/4th sensor later

1. Copy `01_pir_sensor.ino` (the BLE wrapper is identical), keep your sensor
   code in `loop()`.
2. Set `DEVICE_ID "S3"`, `DEVICE_NAME "ESP32-YOURSENSOR-S3"`, and a new
   characteristic UUID (e.g. `…7A0243A3-…`).
3. Flash, power on — the hub connects automatically. **No hub changes.**
   Hub accepts up to 4 sensors (BLE limit of ESP32-WROOM-32).

## 11. Troubleshooting

| Symptom | Fix |
|---|---|
| Hub never finds a sensor | Names must start with `ESP32-`; check Serial Monitor of the sensor shows `Advertising as…`; keep sensors within a few meters |
| `connect_failed` events | Sensors too far / too many Wi-Fi+BLE interference; power sensors from stable 5 V |
| Sensor shows "Hub connected" then nothing on laptop | Laptop is probably also connected to the sensor's serial port — close other monitors; check you opened the **hub's** port |
| `stale` events but link is up | Sensor `loop()` delayed (e.g. long `pulseIn` timeouts); acceptable occasionally, otherwise shorten sensor cycle |
| `gap` events | BLE notify is lossy by design (no retransmit); occasional gaps under interference are normal — SEQ makes them visible |
| ArduinoJson compile error | Install ArduinoJson v6/v7 via Library Manager |
| Multiple COM ports confuse the script | `python3 hub_receiver.py --port COM5` (or `/dev/ttyUSB0`) |
| Want >4 sensors | Use a second hub, or an ESP32-S3 hub variant (more ACL connections) |

## 12. Original source attribution

HC-SR04 sketch based on Rui Santos / RandomNerdTutorials
(https://RandomNerdTutorials.com/esp32-hc-sr04-ultrasonic-arduino/),
permission notice preserved in the sketch header.
