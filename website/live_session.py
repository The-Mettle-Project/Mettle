import ctypes
import difflib
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from ctypes import wintypes

RECORDS = "6000000"
PAUSE = "10"
WAIT_SECONDS = 6
CUT = "⋯"

FIX = [(
    "    var merchant: int64 = merchant_of(record);\n",
    "    var merchant: int64 = merchant_of(record);\n"
    "    if (merchant >= 64) {\n"
    "        println(\"record {record} held: merchant {merchant} does not exist\");\n"
    "        return;\n"
    "    }\n",
)]

FIELD = [
    ("    count: int64\n}", "    count: int64\n    refunds: int64\n}"),
    (
        "    book[merchant].count = book[merchant].count + 1;\n",
        "    book[merchant].count = book[merchant].count + 1;\n"
        "    if (cents < 0) {\n"
        "        book[merchant].refunds = book[merchant].refunds + 1;\n"
        "    }\n",
    ),
    ("    var settled: int64 = 0;\n", "    var settled: int64 = 0;\n    var refunds: int64 = 0;\n"),
    (
        "        settled = settled + book[m].sales;\n",
        "        settled = settled + book[m].sales;\n        refunds = refunds + book[m].refunds;\n",
    ),
    (
        "    println(\"{done} records, {settled / 100} dollars settled\");",
        "    println(\"{done} records, {settled / 100} dollars settled, {refunds} refunds\");",
    ),
]


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
kernel32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]


def child_of(parent, name):
    snapshot = kernel32.CreateToolhelp32Snapshot(2, 0)
    entry = ProcessEntry()
    entry.dwSize = ctypes.sizeof(ProcessEntry)
    found = 0
    ok = kernel32.Process32FirstW(snapshot, ctypes.byref(entry))
    while ok:
        if entry.th32ParentProcessID == parent and entry.szExeFile.lower() == name:
            found = entry.th32ProcessID
            break
        ok = kernel32.Process32NextW(snapshot, ctypes.byref(entry))
    kernel32.CloseHandle(snapshot)
    return found


def read_eight(handle, address):
    buffer = ctypes.create_string_buffer(8)
    got = ctypes.c_size_t(0)
    if not kernel32.ReadProcessMemory(handle, ctypes.c_void_p(address), buffer, 8, ctypes.byref(got)) or got.value != 8:
        sys.exit("live_session.py: could not read the entry of post")
    return buffer.raw


def decode(raw, address):
    if raw[0] == 0xE9:
        rel = int.from_bytes(raw[1:5], "little", signed=True)
        return "jmp 0x{:X}".format(address + 5 + rel)
    return "nop"


def apply_edit(path, pairs):
    with open(path, "rb") as f:
        text = f.read().decode("utf-8")
    for old, new in pairs:
        if old not in text:
            sys.exit("live_session.py: settle.mettle no longer contains: " + old)
        text = text.replace(old, new, 1)
    with open(path, "wb") as f:
        f.write(text.encode("utf-8"))
    return text


def added_lines(before, after):
    a = before.split("\n")
    b = after.split("\n")
    out = []
    for tag, _, _, j1, j2 in difflib.SequenceMatcher(None, a, b).get_opcodes():
        if tag in ("replace", "insert"):
            out.extend(range(j1 + 1, j2 + 1))
    return out


def record(repo, mettle, work):
    cwd = os.path.join(work, "live")
    shutil.rmtree(cwd, ignore_errors=True)
    os.makedirs(cwd)
    src = os.path.join(cwd, "settle.mettle")
    shutil.copyfile(os.path.join(repo, "examples", "settle", "settle.mettle"), src)
    with open(src, "rb") as f:
        base = f.read().decode("utf-8").replace("\r\n", "\n")

    prefix = os.path.normpath(cwd) + os.sep
    args = ["live", "settle.mettle", "-s", "--", RECORDS, PAUSE]
    t0 = time.monotonic()
    lines = []
    entries = []
    lock = threading.Lock()
    target = {"handle": None, "address": 0}
    proc = subprocess.Popen([mettle] + args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def sample():
        raw = read_eight(target["handle"], target["address"])
        entries.append({"t": time.monotonic() - t0, "bytes": raw.hex(" "), "means": decode(raw, target["address"])})

    def reader():
        for chunk in proc.stdout:
            text = chunk.decode("utf-8", "replace").rstrip().replace(prefix, "")
            now = time.monotonic() - t0
            with lock:
                lines.append({"t": now, "s": text, "k": ""})
            if text.startswith("[live] patch ") and target["handle"]:
                sample()

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    def await_line(pattern, after=0, timeout=120):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            with lock:
                for i in range(after, len(lines)):
                    if re.search(pattern, lines[i]["s"]):
                        return i
            if proc.poll() is not None and not thread.is_alive():
                break
            time.sleep(0.002)
        proc.kill()
        sys.exit("live_session.py: never saw /" + pattern + "/ from mettle live")

    def note(text, kind):
        with lock:
            lines.append({"t": time.monotonic() - t0, "s": text, "k": kind})
            return len(lines) - 1

    started = await_line(r"^\[live\] running")
    pid = 0
    for _ in range(500):
        pid = child_of(proc.pid, "settle.exe")
        if pid:
            break
        time.sleep(0.004)
    if not pid:
        proc.kill()
        sys.exit("live_session.py: no settle.exe under mettle live")
    target["handle"] = kernel32.OpenProcess(0x0010 | 0x0400, False, pid)
    state = open(os.path.join(cwd, "settle.exe.live"), "rb").read().decode("utf-8", "replace")
    image = int(re.search(r"^image_base (\d+)", state, re.M).group(1))
    target["address"] = image + int(re.search(r"^s (\d+) post$", state, re.M).group(1))
    sample()

    fault = await_line(r"^Fatal error")
    await_line(r"main stopped at the fault", fault)
    time.sleep(WAIT_SECONDS)
    fix_saved = note("saved settle.mettle", "save")
    fix_text = apply_edit(src, FIX)
    resumed = await_line(r"main continues from", fix_saved)

    await_line(r"^3000000 records", resumed)
    field_saved = note("saved settle.mettle", "save")
    field_text = apply_edit(src, FIELD)
    moved = await_line(r"main moves to the current code", field_saved)

    await_line(r"^4500000 records", moved)
    undo_at = note("mettle --live-undo settle.exe", "shell")
    undo = subprocess.run([mettle, "--live-undo", "settle.exe"], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if undo.returncode != 0:
        proc.kill()
        sys.exit("live_session.py: --live-undo failed: " + undo.stdout.decode())
    note(undo.stdout.decode().strip(), "shell-out")
    undone = await_line(r"^\[live\] patch 3 undoes patch 2", undo_at)
    undo_moved = await_line(r"main moves to the current code", undone)

    try:
        proc.wait(timeout=180)
    except subprocess.TimeoutExpired:
        proc.kill()
        sys.exit("live_session.py: the job did not finish")
    thread.join(5)
    if proc.returncode != 0:
        sys.exit("live_session.py: mettle live exited with " + str(proc.returncode))
    kernel32.CloseHandle(target["handle"])

    build = subprocess.run([mettle, "--build", "settle.mettle", "-o", "clean.exe"], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if build.returncode != 0:
        sys.exit("live_session.py: the clean build failed: " + build.stdout.decode())
    began = time.monotonic()
    clean = subprocess.run([os.path.join(cwd, "clean.exe"), RECORDS], cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    clean_seconds = time.monotonic() - began
    clean_lines = clean.stdout.decode().replace("\r\n", "\n").strip().split("\n")

    session_text = "\n".join(l["s"] for l in lines)
    session_sum = re.search(r"^checksum (\d+)$", session_text, re.M).group(1)
    clean_sum = re.search(r"^checksum (\d+)$", "\n".join(clean_lines), re.M).group(1)
    if session_sum != clean_sum:
        sys.exit("live_session.py: checksums differ: session " + session_sum + ", clean " + clean_sum)
    pattern = RECORDS + r" records, (\d+) dollars settled"
    session_dollars = re.search(pattern, session_text).group(1)
    clean_dollars = re.search(pattern, "\n".join(clean_lines)).group(1)
    if session_dollars != clean_dollars:
        sys.exit("live_session.py: dollars differ: session " + session_dollars + ", clean " + clean_dollars)

    origin = lines[started]["t"]
    exit_line = next(i for i, l in enumerate(lines) if " exited with 0" in l["s"])
    duration = lines[exit_line]["t"] - origin

    points = [[0.0, 0]]
    for l in lines:
        m = re.match(r"^(\d+) records", l["s"])
        if m:
            points.append([l["t"] - origin, int(m.group(1))])
            if int(m.group(1)) == 1500000:
                points.append([lines[resumed]["t"] - origin, 1500000])
    if len(points) < 6:
        sys.exit("live_session.py: too few reports to plot")

    rel = lambda i: lines[i]["t"] - origin
    events = [
        {"n": 1, "t": rel(fault)},
        {"n": 2, "t": rel(resumed)},
        {"n": 3, "t": rel(moved)},
        {"n": 4, "t": rel(undone)},
        {"n": 5, "t": duration},
    ]

    dropped = (r"^Stack trace", r"^  #\d", r"running frames of", r"\[live\] main moves")
    shown = []
    cutting = False
    for i, l in enumerate(lines):
        when = max(0.0, l["t"] - origin)
        if any(re.search(p, l["s"]) for p in dropped):
            if not cutting:
                shown.append({"t": when, "s": "  " + CUT, "k": ""})
            cutting = True
            continue
        cutting = False
        shown.append({"t": when, "s": l["s"], "k": l["k"]})
        if i in (fix_saved, field_saved):
            text = fix_text if i == fix_saved else field_text
            marks = added_lines(base, fix_text) if i == fix_saved else added_lines(fix_text, field_text)
            rows = text.split("\n")
            for k, n in enumerate(marks):
                if k and n != marks[k - 1] + 1:
                    shown.append({"t": when, "s": "  " + CUT, "k": ""})
                shown.append({"t": when, "s": rows[n - 1].strip(), "k": "add"})
    shown.append({"t": duration, "s": "mettle --build settle.mettle -o clean.exe", "k": "shell"})
    shown.append({"t": duration, "s": "./clean.exe " + RECORDS, "k": "shell"})
    shown.append({"t": duration, "s": "  " + CUT, "k": "shell-out"})
    for l in clean_lines[-2:]:
        shown.append({"t": duration, "s": l, "k": "shell-out"})

    samples = [dict(e, t=max(0.0, e["t"] - origin)) for e in entries]
    if samples[0]["bytes"] != "0f 1f 84 00 00 00 00 00":
        sys.exit("live_session.py: post does not start with the eight-byte no-op: " + samples[0]["bytes"])
    if len(samples) < 4 or any(not s["bytes"].startswith("e9") for s in samples[1:]):
        sys.exit("live_session.py: a patch did not leave a jump at post")

    return {
        "command": "mettle " + " ".join(args),
        "duration": duration,
        "lines": shown,
        "points": points,
        "events": events,
        "entries": samples,
        "address": "0x{:X}".format(target["address"]),
        "plateau": [rel(fault), rel(resumed)],
        "facts": {
            "{{LIVE_CMD}}": "mettle " + " ".join(args),
            "{{LIVE_WAIT}}": str(WAIT_SECONDS),
            "{{LIVE_PAUSE}}": PAUSE,
            "{{LIVE_FIX_MS}}": str(round((lines[resumed]["t"] - lines[fix_saved]["t"]) * 1000)),
            "{{LIVE_FIELD_MS}}": str(round((lines[moved]["t"] - lines[field_saved]["t"]) * 1000)),
            "{{LIVE_UNDO_MS}}": str(round((lines[undo_moved]["t"] - lines[undo_at]["t"]) * 1000)),
            "{{LIVE_CHECKSUM}}": session_sum,
            "{{LIVE_CLEAN_SECONDS}}": "{:.2f}".format(clean_seconds),
        },
    }
