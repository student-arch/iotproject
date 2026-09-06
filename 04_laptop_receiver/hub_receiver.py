#!/usr/bin/env python3
"""
ESP32 BLE Sensor Hub - Laptop side receiver (single USB/serial connection).

Usage:
    pip install pyserial
    python3 hub_receiver.py                 # auto-detect the hub port
    python3 hub_receiver.py --port /dev/ttyUSB0
    python3 hub_receiver.py --log hub.jsonl # also save raw JSON lines

The hub prints one JSON object per sensor packet, e.g.
    {"src":"esp32hub","id":"S1","type":"PIR","seq":42,"val":"1"}
This script pretty-prints them and optionally appends to a .jsonl log.
"""
import argparse
import json
import sys
import time

import serial                    # pyserial
from serial.tools import list_ports

HUB_BAUD = 115200


def auto_detect_port():
    """Pick the first plausible USB-serial device (hub must be the only one)."""
    candidates = [p for p in list_ports.comports()
                  if p.vid and p.pid]          # USB CDC/FTDI/etc.
    for p in candidates:
        print(f"[i] found serial device: {p.device}  ({p.description})")
    if not candidates:
        print("[!] no USB serial device found. Plug in the hub.")
        sys.exit(1)
    if len(candidates) > 1:
        print("[i] multiple devices found - use --port to pick the hub")
    return candidates[0].device


def main():
    ap = argparse.ArgumentParser(description="ESP32 BLE Sensor Hub receiver")
    ap.add_argument("--port", default=None, help="serial port, e.g. /dev/ttyUSB0 or COM5")
    ap.add_argument("--baud", type=int, default=HUB_BAUD)
    ap.add_argument("--log", default=None, help="append raw JSON lines to this file")
    args = ap.parse_args()

    port = args.port or auto_detect_port()
    print(f"[i] opening hub on {port} @ {args.baud}")

    logf = open(args.log, "a", encoding="utf-8") if args.log else None

    try:
        with serial.Serial(port, args.baud, timeout=2) as ser:
            buf = b""
            garbage = 0                # consecutive bad/failed lines
            last_line, last_rep = None, 0
            while True:
                buf += ser.read(256)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    ts = time.strftime("%H:%M:%S")

                    # resync: if we joined the stream mid-line, drop the
                    # garbage before the first '{' (banner text passes as-is)
                    i = line.find(b"{")
                    if i > 0:
                        line = line[i:]
                    if not line.startswith(b"{"):
                        garbage += 1
                        continue

                    text = line.decode("utf-8", errors="replace")

                    # flood guard: identical line repeated many times in a
                    # row = host-side serial artifact -> purge and resync
                    if text == last_line:
                        last_rep += 1
                        if last_rep == 10:
                            print(f"{ts}  [!] identical line x10 - purging input buffer")
                            ser.reset_input_buffer()
                            buf = b""
                        if last_rep > 10:
                            continue
                    else:
                        last_line, last_rep = text, 0

                    try:
                        obj = json.loads(text)
                    except json.JSONDecodeError:
                        garbage += 1
                        if garbage <= 3:
                            print(f"{ts}  [unparsed] {text}")
                        if garbage == 50:
                            print(f"{ts}  [!] 50 bad lines - purging input buffer")
                            ser.reset_input_buffer()
                            buf = b""
                        continue

                    garbage = 0
                    print(f"{ts}  {json.dumps(obj, ensure_ascii=False)}")
                    if logf:
                        logf.write(json.dumps({"ts": time.time(), **obj}) + "\n")
                        logf.flush()
    except KeyboardInterrupt:
        print("\n[i] stopped")
    except serial.SerialException as e:
        print(f"[!] serial error: {e}")


if __name__ == "__main__":
    main()
