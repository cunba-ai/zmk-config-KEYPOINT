#!/usr/bin/env python3
"""Set the clock shown on the KEYPOINT right-half screen.

Usage:
    python set_clock.py            # scan all serial ports and set the clock
    python set_clock.py COM7       # only use the given port

The keyboard has no RTC, so the right-half firmware listens on its USB CDC
serial port for a line "T<epoch>\n" (epoch expressed in *local* wall-clock
seconds) and replies "OK MM-DD HH:MM". Plug the RIGHT half of the keyboard
into USB (its own USB-C port), then run this script.

Requires: pip install pyserial

Notes:
  * The clock is saved to flash every 10 minutes, so a power cycle loses at
    most the time the board was powered off (plus small clock drift).
  * Re-run this script occasionally to re-sync.
"""

import calendar
import sys
import time

try:
    import serial  # pyserial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required:  pip install pyserial")


def local_epoch() -> int:
    # Local wall-clock time expressed as if it were UTC epoch seconds, so the
    # firmware can render date/hours directly without timezone handling.
    return calendar.timegm(time.localtime())


def try_port(port: str, epoch: int) -> bool:
    try:
        with serial.Serial(port, 115200, timeout=1) as ser:
            ser.reset_input_buffer()
            ser.write(f"T{epoch}\n".encode())
            reply = ser.read(64)
    except (serial.SerialException, OSError):
        return False
    text = reply.decode(errors="replace").strip()
    if text.startswith("OK"):
        print(f"  {port}: clock set -> {text}")
        return True
    return False


def main() -> None:
    epoch = local_epoch()
    print(f"local time epoch: {epoch} ({time.strftime('%Y-%m-%d %H:%M')})")

    ports = [sys.argv[1]] if len(sys.argv) > 1 else [p.device for p in list_ports.comports()]
    if not ports:
        sys.exit("no serial ports found - plug in the RIGHT half via USB and retry")

    for port in ports:
        print(f"  trying {port} ...", end=" ", flush=True)
        if try_port(port, epoch):
            print("done")
            return
        print("no response")
    sys.exit("no keyboard answered - make sure the RIGHT half (the one with the "
             "screen/clock) is plugged in via USB; the left half also shows a "
             "serial port but ignores clock commands")


if __name__ == "__main__":
    main()
