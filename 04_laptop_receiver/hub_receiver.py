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
            while True:
                line = ser.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                ts = time.strftime("%H:%M:%S")

                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    print(f"{ts}  {line}")          # boot banner / debug text
                    continue

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
