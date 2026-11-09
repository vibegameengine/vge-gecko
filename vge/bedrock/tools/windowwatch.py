import ctypes
import hashlib
import threading
import time
from ctypes import wintypes as W

from PIL import ImageGrab

user32 = ctypes.WinDLL("user32", use_last_error=True)
ENUM = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
user32.WindowFromPoint.argtypes = [W.POINT]
user32.WindowFromPoint.restype = W.HWND
user32.GetAncestor.argtypes = [W.HWND, W.UINT]
user32.GetAncestor.restype = W.HWND
GA_ROOT = 2


def main_window(pid):
    found = []

    def visit(hwnd, _):
        owner = W.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        rect = W.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        if owner.value == pid and user32.IsWindowVisible(hwnd) and rect.right - rect.left > 200:
            found.append(hwnd)
        return True

    user32.EnumWindows(ENUM(visit), 0)
    return found[0] if found else None


def client_rect(hwnd):
    rect = W.RECT()
    user32.GetClientRect(hwnd, ctypes.byref(rect))
    origin = W.POINT(0, 0)
    user32.ClientToScreen(hwnd, ctypes.byref(origin))
    return origin.x, origin.y, origin.x + rect.right, origin.y + rect.bottom


class WindowWatch:
    def __init__(self, pid, period=2.0, shots=None):
        self.shots = shots
        self.pid = pid
        self.period = period
        self.samples = []
        self.stop_flag = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)

    def start(self):
        self.thread.start()

    def stop(self):
        self.stop_flag.set()
        self.thread.join(timeout=10)

    def run(self):
        start = time.time()
        previous = None
        while not self.stop_flag.wait(self.period):
            hwnd = main_window(self.pid)
            if not hwnd:
                continue
            box = client_rect(hwnd)
            center = W.POINT((box[0] + box[2]) // 2, (box[1] + box[3]) // 2)
            top = user32.WindowFromPoint(center)
            root = user32.GetAncestor(top, GA_ROOT) if top else None
            digest = None
            try:
                full = ImageGrab.grab(bbox=box, all_screens=True)
                if self.shots and len(self.samples) % 5 == 0:
                    full.convert("RGB").resize((640, 360)).save(f"{self.shots}-{len(self.samples):03d}.jpg")
                image = full.resize((160, 90))
                digest = hashlib.md5(image.tobytes()).hexdigest()
            except OSError:
                pass
            self.samples.append(
                {
                    "t": round(time.time() - start, 1),
                    "changed": digest is not None and digest != previous,
                    "foreground": user32.GetForegroundWindow() == hwnd,
                    "on_top_at_center": root == hwnd,
                    "iconic": bool(user32.IsIconic(hwnd)),
                }
            )
            previous = digest

    def summary(self):
        visible = [s for s in self.samples if s["on_top_at_center"] and not s["iconic"]]
        frozen = [s["t"] for s in visible if not s["changed"]]
        return {
            "samples": len(self.samples),
            "visible_samples": len(visible),
            "frozen_visible_samples": len(frozen),
            "first_frozen_at": frozen[0] if frozen else None,
            "frozen_times": frozen[:40],
            "foreground_samples": sum(s["foreground"] for s in self.samples),
        }
