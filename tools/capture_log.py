#!/usr/bin/env python3
"""Capture USB CDC log output from the KEYPOINT keyboard (debug firmware).

Usage:
    python capture_log.py            # auto-detect the keyboard serial port
    python capture_log.py COM7       # use a specific port

Press Ctrl+C to stop. Lines are echoed to the terminal and appended to
keyboard_log.txt in the current directory.

Requires: pip install pyserial
"""

import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")

LOG_FILE = "keyboard_log.txt"


def main() -> None:
    port = sys.argv[1] if len(sys.argv) > 1 else None
    if port is None:
        # The ZMK debug firmware advertises the manufacturer "ZMK Project".
        candidates = [p.device for p in list_ports.comports()]
        if not candidates:
            sys.exit("no serial ports found - plug the keyboard half in via USB")
        port = candidates[0]
        if len(candidates) > 1:
            print(f"multiple ports found {candidates}, using {port}; "
                  f"pass one explicitly to override")

    print(f"listening on {port} (Ctrl+C to stop) ...")
    with serial.Serial(port, 115200, timeout=0.5) as ser, open(LOG_FILE, "a", encoding="utf-8", errors="replace") as f:
        f.write(f"\n===== capture started {time.strftime('%Y-%m-%d %H:%M:%S')} on {port} =====\n")
        while True:
            data = ser.read(4096)
            if data:
                text = data.decode(errors="replace")
                sys.stdout.write(text)
                sys.stdout.flush()
                f.write(text)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print(f"\nsaved to {LOG_FILE}")
