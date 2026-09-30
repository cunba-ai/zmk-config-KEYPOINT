#!/usr/bin/env python3
"""Capture RAW TrackPoint deltas from a native laptop TrackPoint (Windows).

Uses the Raw Input API, which reports unaccelerated device deltas (before
Windows pointer ballistics), so the trace reflects what the TrackPoint
hardware itself outputs.

Usage:
    python capture_tp.py            # capture until Ctrl+C, save tp_trace.csv

During capture, move ONLY the native TrackPoint. Do three strokes of each
kind, a few seconds apart:
  1. very light, slow push (precision aiming)
  2. medium push
  3. firm, fast push (flick across screen)

The CSV (timestamp_ms, dx, dy) is used to calibrate the keyboard's pointer
acceleration curve to match the native feel.

No third-party dependencies (ctypes only).
"""

import csv
import ctypes
import ctypes.wintypes as wt
import sys
import time

WM_INPUT = 0x00FF
RIDEV_INPUTSINK = 0x00000100
RID_INPUT = 0x10000003
RIM_TYPEMOUSE = 0
MOUSE_MOVE_RELATIVE = 0

WNDPROC = ctypes.WINFUNCTYPE(
    ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class RAWINPUTDEVICE(ctypes.Structure):
    _fields_ = [("usUsagePage", ctypes.c_ushort),
                ("usUsage", ctypes.c_ushort),
                ("dwFlags", wt.DWORD),
                ("hwndTarget", wt.HWND)]


class RAWINPUTHEADER(ctypes.Structure):
    _fields_ = [("dwType", wt.DWORD),
                ("dwSize", wt.DWORD),
                ("hDevice", wt.HANDLE),
                ("wParam", wt.WPARAM)]


class RAWMOUSE(ctypes.Structure):
    class _U(ctypes.Union):
        class _S(ctypes.Structure):
            _fields_ = [("usButtonFlags", ctypes.c_ushort),
                        ("usButtonData", ctypes.c_ushort)]
        _fields_ = [("ulButtons", wt.DWORD),
                    ("_s", _S)]
    _fields_ = [("usFlags", ctypes.c_ushort),
                ("u", _U),
                ("ulRawButtons", wt.DWORD),
                ("lLastX", wt.LONG),
                ("lLastY", wt.LONG),
                ("ulExtraInformation", wt.DWORD)]


class RAWINPUT(ctypes.Structure):
    _fields_ = [("header", RAWINPUTHEADER),
                ("mouse", RAWMOUSE)]


user32 = ctypes.windll.user32
kernel32 = ctypes.windll.kernel32

rows = []
devices = {}
t0 = time.perf_counter()
running = True
hwnd = None


@WNDPROC
def wnd_proc(h, msg, wparam, lparam):
    global running
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(lparam, RID_INPUT, None,
                               ctypes.byref(size), ctypes.sizeof(RAWINPUTHEADER))
        buf = ctypes.create_string_buffer(size.value)
        n = user32.GetRawInputData(lparam, RID_INPUT, buf,
                                   ctypes.byref(size),
                                   ctypes.sizeof(RAWINPUTHEADER))
        if n > 0:
            ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
            if ri.header.dwType == RIM_TYPEMOUSE:
                if ri.mouse.usFlags & 1 == MOUSE_MOVE_RELATIVE:
                    dx, dy = ri.mouse.lLastX, ri.mouse.lLastY
                    if dx or dy:
                        dev = ri.header.hDevice
                        devices[dev] = devices.get(dev, 0) + 1
                        rows.append((round((time.perf_counter() - t0) * 1000, 3),
                                     dx, dy, dev & 0xFFFF))
    return user32.DefWindowProcW(h, msg, wparam, lparam)


def device_names():
    out = {}
    for dev, count in devices.items():
        size = wt.UINT(0)
        user32.GetRawInputDeviceInfoW(dev, 0, None, ctypes.byref(size))
        buf = ctypes.create_unicode_buffer(size.value)
        user32.GetRawInputDeviceInfoW(dev, 0, buf, ctypes.byref(size))
        out[buf.value] = count
    return out


def main():
    global hwnd, running
    wc = wt.WNDCLASSW()
    wc.lpfnWndProc = wnd_proc
    wc.lpszClassName = "TpCap"
    if not user32.RegisterClassW(ctypes.byref(wc)):
        sys.exit("RegisterClassW failed")
    hwnd = user32.CreateWindowExW(0, "TpCap", "tpcap", 0, 0, 0, 0, 0,
                                  None, None, None, None)

    rid = RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd)  # generic mouse
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1,
                                          ctypes.sizeof(RAWINPUTDEVICE)):
        sys.exit("RegisterRawInputDevices failed")

    print("capturing raw mouse input (Ctrl+C to stop) ...")
    print("now do 3x each on the native TrackPoint: light/slow push, "
          "medium push, firm fast push")
    msg = wt.MSG()
    try:
        while user32.GetMessageW(ctypes.byref(msg), None, 0, 0) > 0:
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
    except KeyboardInterrupt:
        pass

    out = "tp_trace.csv"
    with open(out, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "dx", "dy", "dev"])
        w.writerows(rows)
    print(f"\n{len(rows)} movement packets -> {out}")
    print("devices seen:")
    for name, count in device_names().items():
        print(f"  {count:7d} packets  {name[:80]}")
    if rows:
        span = rows[-1][0] - rows[0][0]
        dist = sum(abs(r[1]) + abs(r[2]) for r in rows)
        print(f"span {span:.0f} ms, |dx|+|dy| total {dist}")
        print("send tp_trace.csv back for curve calibration")


if __name__ == "__main__":
    main()
