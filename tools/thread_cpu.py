"""CPU time per thread of a running TenchuZ.exe over an interval (Windows): which threads use
the processor, by name (the runtime names its threads; others show their start address).

  python tools/thread_cpu.py [seconds, default 5] [process name, default TenchuZ.exe]
"""
import ctypes
import ctypes.wintypes as wt
import sys
import time

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
THREAD_QUERY_LIMITED_INFORMATION = 0x0800


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_size_t), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


def process_id(name):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    entry = PROCESSENTRY32W(dwSize=ctypes.sizeof(PROCESSENTRY32W))
    ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == name.lower():
            kernel32.CloseHandle(snap)
            return entry.th32ProcessID
        ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return None


def threads(pid):
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    entry = THREADENTRY32(dwSize=ctypes.sizeof(THREADENTRY32))
    result = []
    ok = kernel32.Thread32First(snap, ctypes.byref(entry))
    while ok:
        if entry.th32OwnerProcessID == pid:
            result.append(entry.th32ThreadID)
        ok = kernel32.Thread32Next(snap, ctypes.byref(entry))
    kernel32.CloseHandle(snap)
    return result


def sample(pid):
    times = {}
    for tid in threads(pid):
        handle = kernel32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
        if not handle:
            continue
        created, exited, kernel, user = (wt.FILETIME() for _ in range(4))
        name = ctypes.c_wchar_p()
        description = ""
        if kernel32.GetThreadDescription(handle, ctypes.byref(name)) >= 0 and name.value:
            description = name.value
            kernel32.LocalFree(name)
        if kernel32.GetThreadTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel),
                                   ctypes.byref(user)):
            to = lambda ft: (ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 1e7
            times[tid] = (description or f"thread {tid}", to(kernel) + to(user))
        kernel32.CloseHandle(handle)
    return times


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    pid = process_id(sys.argv[2] if len(sys.argv) > 2 else "TenchuZ.exe")
    if not pid:
        sys.exit("TenchuZ.exe is not running")
    before = sample(pid)
    time.sleep(seconds)
    after = sample(pid)
    rows = [(after[t][1] - before[t][1], after[t][0]) for t in after if t in before]
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print(f"{total / seconds:.2f} CPUs busy over {seconds:.0f} s")
    for used, name in rows[:15]:
        if used > 0.01 * seconds:
            print(f"  {100 * used / seconds:6.1f}%  {name}")


if __name__ == "__main__":
    main()
