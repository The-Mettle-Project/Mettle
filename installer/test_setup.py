import argparse
import ctypes
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import winreg
import zlib
from ctypes import wintypes

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PAGES = ["ready", "upgrade", "everyone", "work", "done", "failed", "remove", "removing", "removed"]
REGISTRY_ROOT = "Software\\MettleSetupTest"
UNINSTALL = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Mettle"
OLD_UNINSTALL = "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\{D9F4D8EF-0B2C-4B37-A9B0-5A6E30C3D2A1}_is1"
WM_CLOSE, WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_KEYDOWN = 0x10, 0x200, 0x201, 0x202, 0x100
failures = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (" " + str(detail) if detail and not ok else ""), flush=True)
    if not ok:
        failures.append(name)


def wipe_key(path):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_ALL_ACCESS) as key:
            while True:
                try:
                    child = winreg.EnumKey(key, 0)
                except OSError:
                    break
                wipe_key(path + "\\" + child)
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
    except FileNotFoundError:
        pass


def value_of(path, name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_ROOT + "\\" + path) as key:
            return winreg.QueryValueEx(key, name)[0]
    except FileNotFoundError:
        return None


def key_exists(path):
    try:
        winreg.CloseKey(winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_ROOT + "\\" + path))
        return True
    except FileNotFoundError:
        return False


def run_silent(exe, *args):
    r = subprocess.run([exe, "--silent", "--registry-root", REGISTRY_ROOT] + list(args),
                       capture_output=True, text=True, timeout=300)
    return r.returncode, r.stdout + r.stderr


def read_index(setup):
    data = open(setup, "rb").read()
    footer = data[-64:]
    if footer[:8] != b"METTLEIN":
        raise SystemExit("no footer in " + setup)
    index_at, index_len = struct.unpack_from("<QQ", footer, 24)
    index = data[index_at:index_at + index_len]
    total, count, version_len = struct.unpack_from("<QIH", index, 0)
    at = 14 + version_len
    version = index[14:at].decode()
    files = {}
    for _ in range(count):
        length = struct.unpack_from("<H", index, at)[0]
        path = index[at + 2:at + 2 + length].decode()
        at += 2 + length
        size, crc = struct.unpack_from("<QI", index, at)
        at += 12
        files[path] = (size, crc)
    return version, files


def wrong_files(folder, files):
    bad = []
    for path, (size, crc) in files.items():
        full = os.path.join(folder, path)
        if not os.path.isfile(full):
            bad.append("missing " + path)
            continue
        data = open(full, "rb").read()
        if len(data) != size or zlib.crc32(data) != crc:
            bad.append("differs " + path)
    return bad


def installed_names(folder):
    names = []
    for base, _, found in os.walk(folder):
        for name in found:
            names.append(os.path.relpath(os.path.join(base, name), folder).replace("\\", "/"))
    return set(names)


def test_resources(setup):
    shell32 = ctypes.WinDLL("shell32")
    shell32.ExtractIconExW.argtypes = [wintypes.LPCWSTR, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, wintypes.UINT]
    shell32.ExtractIconExW.restype = wintypes.UINT
    check("the exe carries an icon", shell32.ExtractIconExW(setup, -1, None, None, 0) >= 1)
    version = ctypes.WinDLL("version")
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p]
    check("the exe carries version info", version.GetFileVersionInfoSizeW(setup, None) > 0)
    data = open(setup, "rb").read()
    check("the exe carries a manifest", b"PerMonitorV2" in data)


def test_pages(setup, work):
    from PIL import Image
    for page in PAGES:
        for scale in ("100", "150", "200"):
            out = os.path.join(work, "%s-%s.bmp" % (page, scale))
            code = subprocess.run([setup, "--registry-root", REGISTRY_ROOT, "--render", page, out, "--scale", scale]).returncode
            ok = code == 0 and os.path.exists(out)
            if ok:
                image = Image.open(out).convert("RGB")
                width, height = image.size
                ok = width == round(900 * int(scale) / 100) and height == round(560 * int(scale) / 100)
                lightest = max(max(band) for band in image.getextrema())
                ok = ok and lightest > 200
                image.save(out[:-4] + ".png")
                os.remove(out)
            check("page %s renders at %s%%" % (page, scale), ok, code)


def test_install(setup, work):
    version, files = read_index(setup)
    target = os.path.join(work, "Program Files", "Mettle Test")
    code, out = run_silent(setup, "--dir", target)
    check("install exits 0", code == 0, out)
    check("install reports the version", version in out, out)
    bad = wrong_files(target, files)
    check("every file arrives byte for byte", not bad, bad[:4])
    extra = installed_names(target) - set(files) - {"uninstall.exe"}
    check("nothing extra is installed", not extra, sorted(extra)[:4])
    check("no staging folders are left", not os.path.exists(target + ".installing") and not os.path.exists(target + ".replaced"))
    check("the uninstall entry names the version", value_of(UNINSTALL, "DisplayVersion") == version)
    check("the uninstall entry names the folder", (value_of(UNINSTALL, "InstallLocation") or "").lower() == target.lower())
    check("the uninstall string names uninstall.exe", "uninstall.exe" in (value_of(UNINSTALL, "UninstallString") or ""))
    path_value = value_of("Environment", "Path") or ""
    bin_dir = os.path.join(target, "bin")
    check("PATH gains the bin folder", bin_dir.lower() in path_value.lower(), path_value)
    check("the association names MettleFile", value_of("Software\\Classes\\.mettle", "") == "MettleFile")
    check("the association has no open command", not key_exists("Software\\Classes\\MettleFile\\shell"))

    hello = os.path.join(work, "hello.mettle")
    with open(hello, "w") as f:
        f.write('import "std/io";\nfn main() -> int32 { println("from the installed compiler"); return 0; }\n')
    exe = os.path.join(work, "hello.exe")
    r = subprocess.run([os.path.join(target, "bin", "mettle.exe"), "--build", hello, "-o", exe], capture_output=True, text=True, cwd=work)
    check("the installed compiler builds a program", r.returncode == 0, r.stdout + r.stderr)
    r = subprocess.run([exe], capture_output=True, text=True)
    check("that program runs", r.stdout.strip() == "from the installed compiler", r.stdout)

    notes = os.path.join(target, "my-notes.txt")
    with open(notes, "w") as f:
        f.write("mine")
    code, out = run_silent(setup, "--dir", target)
    check("installing over itself exits 0", code == 0, out)
    check("a file you added survives", os.path.exists(notes))
    bad = wrong_files(target, files)
    check("files are still byte for byte", not bad, bad[:4])
    path_value = value_of("Environment", "Path") or ""
    check("PATH is not doubled", path_value.lower().count(bin_dir.lower()) == 1, path_value)

    other = os.path.join(work, "Someone Elses Folder")
    os.makedirs(other)
    with open(os.path.join(other, "precious.txt"), "w") as f:
        f.write("x")
    code, out = run_silent(setup, "--dir", other, "--no-path", "--no-association")
    check("a folder holding other files is refused", code == 1, out)
    check("its files are untouched", os.listdir(other) == ["precious.txt"])
    check("no staging folder sits beside it", not os.path.exists(other + ".installing"))

    code, out = run_silent(os.path.join(target, "uninstall.exe"))
    check("uninstall exits 0", code == 0, out)
    deadline = time.time() + 15
    while time.time() < deadline and os.path.exists(os.path.join(target, "uninstall.exe")):
        time.sleep(0.2)
    check("only the added file is left", os.listdir(target) == ["my-notes.txt"], os.listdir(target))
    check("the uninstall entry is gone", not key_exists(UNINSTALL))
    path_value = value_of("Environment", "Path") or ""
    check("PATH loses the bin folder", bin_dir.lower() not in path_value.lower(), path_value)
    check("the association is gone", not key_exists("Software\\Classes\\.mettle"))
    os.remove(notes)
    deadline = time.time() + 15
    copies = ["x"]
    while time.time() < deadline and copies:
        copies = [n for n in os.listdir(tempfile.gettempdir()) if n.startswith("mettle-uninstall-")]
        time.sleep(0.2)
    check("the uninstaller's temp copy deletes itself", not copies, copies)


def test_takeover(setup, work):
    wipe_key(REGISTRY_ROOT)
    old = os.path.join(work, "old install")
    os.makedirs(os.path.join(old, "bin"))
    with open(os.path.join(old, "bin", "mettle.exe"), "w") as f:
        f.write("old")
    with open(os.path.join(old, "old-only.txt"), "w") as f:
        f.write("old")
    marker = os.path.join(work, "old-ran.txt")
    late = os.path.join(work, "old-clone.bat")
    with open(late, "w") as f:
        f.write('@echo off\r\nping -n 3 127.0.0.1 >nul\r\ndel /q "%s"\r\ndel /q "%s"\r\n(goto) 2>nul & del "%%~f0"\r\n'
                % (os.path.join(old, "old-only.txt"), os.path.join(old, "unins000.bat")))
    uninstaller = os.path.join(old, "unins000.bat")
    with open(uninstaller, "w") as f:
        f.write('@echo off\r\necho %%* > "%s"\r\nstart "" /b cmd /c "%s"\r\nexit /b 0\r\n' % (marker, late))
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, REGISTRY_ROOT + "\\" + OLD_UNINSTALL) as key:
        winreg.SetValueEx(key, "InstallLocation", 0, winreg.REG_SZ, old + "\\")
        winreg.SetValueEx(key, "DisplayVersion", 0, winreg.REG_SZ, "0.16.3")
        winreg.SetValueEx(key, "UninstallString", 0, winreg.REG_SZ, '"%s"' % uninstaller)
    began = time.time()
    code, out = run_silent(setup, "--no-path", "--no-association")
    check("an older install's folder is taken over", code == 0, out)
    check("its uninstaller runs silently", os.path.exists(marker) and "VERYSILENT" in open(marker).read())
    check("setup waits for its clone to finish", time.time() - began >= 2 and not os.path.exists(uninstaller))
    check("the old compiler is replaced", os.path.getsize(os.path.join(old, "bin", "mettle.exe")) > 1000000)
    code, out = run_silent(os.path.join(old, "uninstall.exe"))
    check("the takeover uninstalls", code == 0, out)
    time.sleep(4)


def test_window(setup, work):
    from PIL import ImageGrab
    user32 = ctypes.WinDLL("user32")
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    user32.FindWindowW.restype = wintypes.HWND
    user32.FindWindowW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR]
    user32.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
    user32.GetWindowRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.GetClientRect.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.RECT)]
    user32.SetForegroundWindow.argtypes = [wintypes.HWND]

    def find(class_name, timeout=8):
        deadline = time.time() + timeout
        while time.time() < deadline:
            found = user32.FindWindowW(class_name, None)
            if found:
                return found
            time.sleep(0.05)
        return None

    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    user32.EnumWindows.argtypes = [enum_proc, wintypes.LPARAM]
    user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
    user32.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
    user32.IsWindowVisible.argtypes = [wintypes.HWND]

    def owned_dialog(pid, timeout):
        deadline = time.time() + timeout
        while True:
            found = []

            def visit(candidate, _):
                owner = wintypes.DWORD()
                user32.GetWindowThreadProcessId(candidate, ctypes.byref(owner))
                name = ctypes.create_unicode_buffer(32)
                user32.GetClassNameW(candidate, name, 32)
                if owner.value == pid and name.value == "#32770" and user32.IsWindowVisible(candidate):
                    found.append(candidate)
                return True

            user32.EnumWindows(enum_proc(visit), 0)
            if found:
                return found[0]
            if time.time() >= deadline:
                return None
            time.sleep(0.05)

    def outer(window):
        r = wintypes.RECT()
        user32.GetWindowRect(window, ctypes.byref(r))
        return r

    def click(window, x, y):
        r = outer(window)
        scale = (r.right - r.left) / 900.0
        position = int(y * scale) << 16 | int(x * scale)
        user32.PostMessageW(window, WM_MOUSEMOVE, 0, position)
        time.sleep(0.15)
        user32.PostMessageW(window, WM_LBUTTONDOWN, 1, position)
        time.sleep(0.03)
        user32.PostMessageW(window, WM_LBUTTONUP, 0, position)

    def shot(window, name):
        user32.SetForegroundWindow(window)
        time.sleep(0.35)
        r = outer(window)
        ImageGrab.grab(bbox=(r.left - 18, r.top - 18, r.right + 18, r.bottom + 18), all_screens=True).save(os.path.join(work, name + ".png"))

    wipe_key(REGISTRY_ROOT)
    target = os.path.join(work, "Window Test", "Mettle")
    process = subprocess.Popen([setup, "--registry-root", REGISTRY_ROOT, "--dir", target])
    window = find("MettleSetup")
    check("the window opens", window is not None)
    time.sleep(0.7)
    client = wintypes.RECT()
    user32.GetClientRect(window, ctypes.byref(client))
    r = outer(window)
    check("no title bar: the client area fills the window",
          client.right == r.right - r.left and client.bottom == r.bottom - r.top,
          (client.right, client.bottom, r.right - r.left, r.bottom - r.top))
    shot(window, "window-ready")

    click(window, 660, 345)
    dialog = owned_dialog(process.pid, 6)
    check("Change opens the folder picker", dialog is not None)
    if dialog:
        time.sleep(0.5)
        shot(dialog, "window-picker")
        user32.PostMessageW(dialog, WM_CLOSE, 0, 0)
        deadline = time.time() + 6
        while time.time() < deadline and owned_dialog(process.pid, 0):
            time.sleep(0.1)
        check("the folder picker closes", owned_dialog(process.pid, 0) is None)
        time.sleep(0.4)
    check("the window survives the picker", process.poll() is None)

    click(window, 700, 501)
    time.sleep(0.35)
    shot(window, "window-installing")
    click(window, 700, 501)
    time.sleep(1.2)
    check("cancel removes staging", not os.path.exists(target + ".installing"))
    check("cancel leaves no folder", not os.path.exists(target))
    check("cancel registers nothing", not key_exists(UNINSTALL))
    check("the window stays open after cancel", process.poll() is None)

    click(window, 700, 501)
    deadline = time.time() + 40
    while time.time() < deadline and not key_exists(UNINSTALL):
        time.sleep(0.1)
    time.sleep(1.6)
    check("the install finishes", key_exists(UNINSTALL))
    shot(window, "window-done")
    user32.PostMessageW(window, WM_KEYDOWN, 27, 0)
    process.wait(timeout=10)
    check("escape closes with exit 0", process.returncode == 0, process.returncode)

    remover = subprocess.Popen([os.path.join(target, "uninstall.exe"), "--registry-root", REGISTRY_ROOT])
    remover.wait(timeout=10)
    window = find("MettleSetup")
    check("the uninstaller opens from its temp copy", window is not None)
    time.sleep(0.7)
    shot(window, "window-remove")
    click(window, 700, 501)
    deadline = time.time() + 20
    while time.time() < deadline and key_exists(UNINSTALL):
        time.sleep(0.1)
    time.sleep(1.2)
    shot(window, "window-removed")
    user32.PostMessageW(window, WM_KEYDOWN, 27, 0)
    time.sleep(5)
    check("the folder is gone", not os.path.exists(target))
    check("the uninstall entry is gone", not key_exists(UNINSTALL))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--setup", default=os.path.join(HERE, "out", "Mettle-Setup.exe"))
    parser.add_argument("--work", default=None)
    parser.add_argument("--window", action="store_true")
    args = parser.parse_args()
    setup = os.path.abspath(args.setup)
    work = os.path.abspath(args.work or tempfile.mkdtemp(prefix="mettle-setup-test-"))
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    wipe_key(REGISTRY_ROOT)
    try:
        test_resources(setup)
        test_pages(setup, work)
        test_install(setup, work)
        test_takeover(setup, work)
        if args.window:
            test_window(setup, work)
    finally:
        wipe_key(REGISTRY_ROOT)
    print("failures:", failures)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
