#!/usr/bin/env python3
"""Run raven.exe on a hidden desktop for automated tests, so it never shows a window, takes focus or input.

  python tools/hidden_run.py [--seconds 60] [--log tmp/run.txt] [--env K=V ...] [-- raven args]

The process is created on its own desktop (CreateDesktop + STARTUPINFO.lpDesktop, the same approach as
TESConversion's tools/live/hidden_launch.py), with stdout/stderr going to --log. It is killed after --seconds.
RAVEN_MUTE=1 and RAVEN_HEADLESS=1 are always set (headless: no dialogs, and real controllers are ignored, so a test
never rumbles or reads the user's controllers; only the script's virtual pad is used); screenshots come from RAVEN_SHOT (glReadPixels), which works
on the hidden desktop.

  --resize SEC:W:H   at SEC seconds, resize the game window's client area to WxH from outside (SetWindowPos, as a
                     user drag ends up doing); repeatable
  --winlog SEC       print the game window's style, client size and position every SEC seconds
"""
import argparse, ctypes, os, sys, time
from ctypes import wintypes

K32 = ctypes.WinDLL("kernel32", use_last_error=True)
U32 = ctypes.WinDLL("user32", use_last_error=True)

CREATE_UNICODE_ENVIRONMENT = 0x400
CREATE_NO_WINDOW = 0x08000000
GENERIC_ALL = 0x10000000
STARTF_USESTDHANDLES = 0x100
GENERIC_WRITE = 0x40000000
FILE_SHARE_READ = 1
CREATE_ALWAYS = 2
FILE_ATTRIBUTE_NORMAL = 0x80
HANDLE_FLAG_INHERIT = 1


class StartupInfo(ctypes.Structure):
    _fields_ = (("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
                ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
                ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
                ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD), ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD),
                ("lpReserved2", ctypes.c_void_p), ("hStdInput", wintypes.HANDLE),
                ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE))


class ProcessInformation(ctypes.Structure):
    _fields_ = (("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
                ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD))


class SecurityAttributes(ctypes.Structure):
    _fields_ = (("nLength", wintypes.DWORD), ("lpSecurityDescriptor", ctypes.c_void_p),
                ("bInheritHandle", wintypes.BOOL))


U32.CreateDesktopW.restype = wintypes.HANDLE
U32.CreateDesktopW.argtypes = (wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD,
                               wintypes.DWORD, ctypes.c_void_p)
U32.CloseDesktop.argtypes = (wintypes.HANDLE,)
K32.CreateFileW.restype = wintypes.HANDLE
K32.CreateFileW.argtypes = (wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                            wintypes.DWORD, wintypes.HANDLE)
K32.CreateProcessW.argtypes = (wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p, wintypes.BOOL,
                               wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR, ctypes.c_void_p, ctypes.c_void_p)
K32.WaitForSingleObject.argtypes = (wintypes.HANDLE, wintypes.DWORD)
K32.TerminateProcess.argtypes = (wintypes.HANDLE, wintypes.UINT)
K32.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
K32.CloseHandle.argtypes = (wintypes.HANDLE,)


def check(ok, what):
    if not ok:
        raise OSError(ctypes.get_last_error(), what + " failed")
    return ok


U32.EnumDesktopWindows.argtypes = (wintypes.HANDLE, ctypes.c_void_p, wintypes.LPARAM)
U32.GetWindowThreadProcessId.argtypes = (wintypes.HWND, ctypes.POINTER(wintypes.DWORD))
U32.GetWindowLongPtrW.restype = ctypes.c_ssize_t
U32.GetWindowLongPtrW.argtypes = (wintypes.HWND, ctypes.c_int)
U32.SetWindowPos.argtypes = (wintypes.HWND, wintypes.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, wintypes.UINT)
U32.AdjustWindowRectEx.argtypes = (ctypes.POINTER(wintypes.RECT), wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
ENUM_PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
WS_CAPTION, WS_THICKFRAME, WS_POPUP = 0x00C00000, 0x00040000, 0x80000000


def find_window(desk, pid):
    """The visible SDL window of process pid on the desktop, or 0."""
    found = []

    def visit(hwnd, _):
        p = wintypes.DWORD()
        U32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
        name = ctypes.create_unicode_buffer(64)
        U32.GetClassNameW(hwnd, name, 64)
        if p.value == pid and name.value == "SDL_app" and U32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return not found

    U32.EnumDesktopWindows(desk, ENUM_PROC(visit), 0)
    return found[0] if found else 0


class DevMode(ctypes.Structure):
    _fields_ = (("dmDeviceName", wintypes.WCHAR * 32), ("dmSpecVersion", wintypes.WORD), ("dmDriverVersion", wintypes.WORD),
                ("dmSize", wintypes.WORD), ("dmDriverExtra", wintypes.WORD), ("dmFields", wintypes.DWORD),
                ("dmPosition", wintypes.POINTL), ("dmDisplayOrientation", wintypes.DWORD),
                ("dmDisplayFixedOutput", wintypes.DWORD), ("dmColor", ctypes.c_short), ("dmDuplex", ctypes.c_short),
                ("dmYResolution", ctypes.c_short), ("dmTTOption", ctypes.c_short), ("dmCollate", ctypes.c_short),
                ("dmFormName", wintypes.WCHAR * 32), ("dmLogPixels", wintypes.WORD), ("dmBitsPerPel", wintypes.DWORD),
                ("dmPelsWidth", wintypes.DWORD), ("dmPelsHeight", wintypes.DWORD), ("dmDisplayFlags", wintypes.DWORD),
                ("dmDisplayFrequency", wintypes.DWORD), ("pad", ctypes.c_byte * 64))


def display_mode():
    """The primary display's current mode (what an exclusive-fullscreen switch would change)."""
    dm = DevMode()
    dm.dmSize = ctypes.sizeof(DevMode) - 64
    U32.EnumDisplaySettingsW(None, -1, ctypes.byref(dm))  # ENUM_CURRENT_SETTINGS
    return f"{dm.dmPelsWidth}x{dm.dmPelsHeight}@{dm.dmDisplayFrequency}Hz"


def describe(hwnd):
    style = U32.GetWindowLongPtrW(hwnd, -16) & 0xFFFFFFFF
    r, c = wintypes.RECT(), wintypes.RECT()
    U32.GetWindowRect(hwnd, ctypes.byref(r))
    U32.GetClientRect(hwnd, ctypes.byref(c))
    kind = "popup" if style & WS_POPUP else "overlapped"
    return (f"window {kind}{' caption' if style & WS_CAPTION else ''}{' sizable' if style & WS_THICKFRAME else ''} "
            f"at ({r.left},{r.top})-({r.right},{r.bottom}), client {c.right}x{c.bottom}")


def resize_client(hwnd, w, h):
    style = U32.GetWindowLongPtrW(hwnd, -16) & 0xFFFFFFFF
    ex = U32.GetWindowLongPtrW(hwnd, -20) & 0xFFFFFFFF
    r = wintypes.RECT(0, 0, w, h)
    U32.AdjustWindowRectEx(ctypes.byref(r), style, False, ex)
    ok = U32.SetWindowPos(hwnd, None, 0, 0, r.right - r.left, r.bottom - r.top, 0x0002 | 0x0004 | 0x0010)
    return "ok" if ok else f"failed ({ctypes.get_last_error()})"


def main():
    ctypes.windll.user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: real pixels
    root = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--log", default=os.path.join(root, "tmp", "run.txt"))
    ap.add_argument("--exe", default=os.path.join(root, "build", "raven.exe"))
    ap.add_argument("--env", action="append", default=[], help="K=V, repeatable")
    ap.add_argument("--desktop", default="RavenTest")
    ap.add_argument("--resize", action="append", default=[], help="SEC:W:H (client size), repeatable")
    ap.add_argument("--winlog", type=float, default=0)
    ap.add_argument("--res", default="1720x720",
                    help="test resolution, windowed (RAVEN_TEST_RES; 720p tall, ultrawide 2.39:1); 'native' keeps the config's")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()

    env = dict(os.environ)
    env.update(RAVEN_MUTE="1", RAVEN_HEADLESS="1")
    if a.res != "native":
        env["RAVEN_TEST_RES"] = a.res
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    block_text = "".join(f"{k}={v}\0" for k, v in sorted(env.items())) + "\0"
    block = ctypes.create_unicode_buffer(block_text, len(block_text))

    os.makedirs(os.path.dirname(os.path.abspath(a.log)), exist_ok=True)
    sa = SecurityAttributes(ctypes.sizeof(SecurityAttributes), None, True)
    log = check(K32.CreateFileW(os.path.abspath(a.log), GENERIC_WRITE, FILE_SHARE_READ, ctypes.byref(sa),
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None), "CreateFileW")
    desk = check(U32.CreateDesktopW(a.desktop, None, None, 0, GENERIC_ALL, None), "CreateDesktopW")
    si = StartupInfo(cb=ctypes.sizeof(StartupInfo), lpDesktop="WinSta0\\" + a.desktop,
                     dwFlags=STARTF_USESTDHANDLES, hStdOutput=log, hStdError=log)
    pi = ProcessInformation()
    exe = os.path.abspath(a.exe)
    cmd = ctypes.create_unicode_buffer(" ".join([f'"{exe}"'] + [f'"{x}"' for x in a.args]))
    try:
        check(K32.CreateProcessW(exe, cmd, None, None, True, CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW, block,
                                 os.path.dirname(exe), ctypes.byref(si), ctypes.byref(pi)), "CreateProcessW")
        print(f"display before: {display_mode()}")
        steps = sorted((float(t), int(w), int(h)) for t, w, h in (r.split(":") for r in a.resize))
        start = time.time()
        next_log = a.winlog
        timed_out = True
        while time.time() - start < a.seconds:
            if K32.WaitForSingleObject(pi.hProcess, 100) == 0:
                timed_out = False
                break
            now = time.time() - start
            while steps and now >= steps[0][0]:
                _, w, h = steps.pop(0)
                hwnd = find_window(desk, pi.dwProcessId)
                print(f"[{now:5.1f}s] resize client to {w}x{h}: {resize_client(hwnd, w, h) if hwnd else 'no window'}")
            if a.winlog and now >= next_log:
                next_log += a.winlog
                hwnd = find_window(desk, pi.dwProcessId)
                if hwnd:
                    print(f"[{now:5.1f}s] {describe(hwnd)}; display {display_mode()}")
        if timed_out:
            K32.TerminateProcess(pi.hProcess, 0)
            K32.WaitForSingleObject(pi.hProcess, 10000)
        code = wintypes.DWORD()
        K32.GetExitCodeProcess(pi.hProcess, ctypes.byref(code))
        print(f"raven.exe {'stopped after timeout' if timed_out else f'exited {code.value:#x}'}; log {a.log}")
    finally:
        for h in (pi.hThread, pi.hProcess, log):
            if h:
                K32.CloseHandle(h)
        U32.CloseDesktop(desk)


if __name__ == "__main__":
    main()
