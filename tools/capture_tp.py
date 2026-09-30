#!/usr/bin/env python3
"""Capture native TrackPoint behaviour on Windows for calibration.

Records two synchronized streams for [lead] + [duration] seconds:
  tp_trace_raw.csv     (t_ms, dx, dy, dev)   raw device deltas, BEFORE
                                             Windows pointer ballistics
  tp_trace_cursor.csv  (t_ms, x, y)          cursor position (AFTER host
                                             acceleration), ~polling rate

Comparing the two streams yields the native machine's input->output transfer
curve, and the raw stream alone characterises the TrackPoint's force->counts
response (report rate, velocity range per stroke strength, noise floor).

Usage:
    python capture_tp.py [lead_s] [duration_s]     (default 15 60)

During the capture window do 3x each on the NATIVE TrackPoint:
  1. very light slow push (precision aiming)
  2. medium push
  3. firm fast push (flick across the screen)

ctypes only, no third-party dependencies.
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
PM_REMOVE = 0x0001

WNDPROC = ctypes.WINFUNCTYPE(ctypes.c_longlong, wt.HWND, wt.UINT, wt.WPARAM, wt.LPARAM)


class WNDCLASSW(ctypes.Structure):
    _fields_ = [("style", wt.UINT),
                ("lpfnWndProc", WNDPROC),
                ("cbClsExtra", ctypes.c_int),
                ("cbWndExtra", ctypes.c_int),
                ("hInstance", wt.HINSTANCE),
                ("hIcon", wt.HICON),
                ("hCursor", ctypes.c_void_p),
                ("hbrBackground", ctypes.c_void_p),
                ("lpszMenuName", wt.LPCWSTR),
                ("lpszClassName", wt.LPCWSTR)]


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


class POINT(ctypes.Structure):
    _fields_ = [("x", wt.LONG), ("y", wt.LONG)]


user32 = ctypes.windll.user32

raw_rows = []
cursor_rows = []
devices = {}
t0 = time.perf_counter()
hwnd = None


@WNDPROC
def wnd_proc(h, msg, wparam, lparam):
    if msg == WM_INPUT:
        size = wt.UINT(0)
        user32.GetRawInputData(lparam, RID_INPUT, None, ctypes.byref(size),
                               ctypes.sizeof(RAWINPUTHEADER))
        buf = ctypes.create_string_buffer(size.value)
        n = user32.GetRawInputData(lparam, RID_INPUT, buf, ctypes.byref(size),
                                   ctypes.sizeof(RAWINPUTHEADER))
        if n > 0:
            ri = ctypes.cast(buf, ctypes.POINTER(RAWINPUT)).contents
            if ri.header.dwType == RIM_TYPEMOUSE and (ri.mouse.usFlags & 1) == 0:
                dx, dy = ri.mouse.lLastX, ri.mouse.lLastY
                if dx or dy:
                    dev = ri.header.hDevice
                    devices[dev] = devices.get(dev, 0) + 1
                    raw_rows.append((round((time.perf_counter() - t0) * 1000, 3),
                                     dx, dy, dev & 0xFFFF))
    return user32.DefWindowProcW(h, msg, wparam, lparam)


def main():
    global hwnd
    lead = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0

    wc = WNDCLASSW()
    wc.lpfnWndProc = wnd_proc
    wc.lpszClassName = "TpCap"
    if not user32.RegisterClassW(ctypes.byref(wc)):
        sys.exit("RegisterClassW failed")
    hwnd = user32.CreateWindowExW(0, "TpCap", "tpcap", 0, 0, 0, 0, 0,
                                  None, None, None, None)
    rid = RAWINPUTDEVICE(1, 2, RIDEV_INPUTSINK, hwnd)
    if not user32.RegisterRawInputDevices(ctypes.byref(rid), 1,
                                          ctypes.sizeof(RAWINPUTDEVICE)):
        sys.exit("RegisterRawInputDevices failed")

    print(f"get ready... capture starts in {lead:.0f}s", flush=True)
    msg = wt.MSG()
    while time.perf_counter() - t0 < lead:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        remaining = lead - (time.perf_counter() - t0)
        if int(remaining) != int(remaining + 0.05) and remaining > 0:
            pass
        time.sleep(0.05)
    for s in range(3, 0, -1):
        print(f"  {s}...", flush=True)
        time.sleep(1)

    print("CAPTURING - do the strokes now: light/slow x3, medium x3, firm/fast x3",
          flush=True)
    capture_end = time.perf_counter() + duration
    pt = POINT()
    last_poll = 0.0
    while time.perf_counter() < capture_end:
        while user32.PeekMessageW(ctypes.byref(msg), None, 0, 0, PM_REMOVE):
            user32.TranslateMessage(ctypes.byref(msg))
            user32.DispatchMessageW(ctypes.byref(msg))
        now = time.perf_counter()
        if now - last_poll >= 0.002:  # 500 Hz cursor polling
            last_poll = now
            if user32.GetCursorPos(ctypes.byref(pt)):
                cursor_rows.append((round((now - t0) * 1000, 3), pt.x, pt.y))
        time.sleep(0.001)

    with open("tp_trace_raw.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "dx", "dy", "dev"])
        w.writerows(raw_rows)
    with open("tp_trace_cursor.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["t_ms", "x", "y"])
        w.writerows(cursor_rows)

    print(f"\ndone: {len(raw_rows)} raw packets, {len(cursor_rows)} cursor samples")
    for dev, count in devices.items():
        size = wt.UINT(0)
        user32.GetRawInputDeviceInfoW(dev, 0, None, ctypes.byref(size))
        buf = ctypes.create_unicode_buffer(size.value)
        user32.GetRawInputDeviceInfoW(dev, 0, buf, ctypes.byref(size))
        print(f"  {count:7d} packets  dev{dev & 0xFFFF:04x}  {buf.value[:70]}")
    if raw_rows:
        print(f"wrote tp_trace_raw.csv + tp_trace_cursor.csv in {sys.path[0] or '.'}")


if __name__ == "__main__":
    main()
