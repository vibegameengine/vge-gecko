import ctypes, json, sys, collections
from ctypes import wintypes as W

k = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
nt = ctypes.WinDLL("ntdll")


class MBI(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_uint64), ("AllocationBase", ctypes.c_uint64), ("AllocationProtect", W.DWORD), ("__a1", W.DWORD),
                ("RegionSize", ctypes.c_uint64), ("State", W.DWORD), ("Protect", W.DWORD), ("Type", W.DWORD), ("__a2", W.DWORD)]


class WSEX(ctypes.Structure):
    _fields_ = [("VirtualAddress", ctypes.c_uint64), ("Attr", ctypes.c_uint64)]


k.OpenProcess.restype = W.HANDLE
k.VirtualQueryEx.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.POINTER(MBI), ctypes.c_size_t]
k.VirtualQueryEx.restype = ctypes.c_size_t
psapi.QueryWorkingSetEx.argtypes = [W.HANDLE, ctypes.c_void_p, W.DWORD]
psapi.GetMappedFileNameW.argtypes = [W.HANDLE, ctypes.c_uint64, W.LPWSTR, W.DWORD]
k.OpenThread.restype = W.HANDLE
k.GetThreadDescription.argtypes = [W.HANDLE, ctypes.POINTER(ctypes.c_wchar_p)]
k.ReadProcessMemory.argtypes = [W.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
nt.NtQueryInformationThread.argtypes = [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.ULONG, ctypes.POINTER(W.ULONG)]

PAGE = 4096


class TE(ctypes.Structure):
    _fields_ = [("dwSize", W.DWORD), ("cntUsage", W.DWORD), ("th32ThreadID", W.DWORD), ("th32OwnerProcessID", W.DWORD),
                ("tpBasePri", W.LONG), ("tpDeltaPri", W.LONG), ("dwFlags", W.DWORD)]


k.CreateToolhelp32Snapshot.restype = W.HANDLE


def threads(pid, hp):
    snap = k.CreateToolhelp32Snapshot(4, 0)
    te = TE()
    te.dwSize = ctypes.sizeof(TE)
    out = []
    ok = k.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k.Thread32Next(snap, ctypes.byref(te))
    k.CloseHandle(snap)
    res = []
    for tid in out:
        h = k.OpenThread(0x0040 | 0x0200 | 0x0800, False, tid)
        if not h:
            res.append({"tid": tid, "name": "?", "base": 0, "limit": 0})
            continue
        name = ctypes.c_wchar_p()
        k.GetThreadDescription(h, ctypes.byref(name))
        n = name.value or ""
        info = (ctypes.c_uint64 * 6)()
        nt.NtQueryInformationThread(h, 0, info, 48, None)
        teb = info[1]
        tib = (ctypes.c_uint64 * 3)()
        got = ctypes.c_size_t()
        k.ReadProcessMemory(hp, teb, tib, 24, ctypes.byref(got))
        start = ctypes.c_uint64()
        nt.NtQueryInformationThread(h, 9, ctypes.byref(start), 8, None)
        res.append({"tid": tid, "name": n, "base": tib[1], "limit": tib[2], "start": start.value})
        k.CloseHandle(h)
    return res


def scan(pid):
    hp = k.OpenProcess(0x0400 | 0x0010, False, pid)
    if not hp:
        return None
    th = threads(pid, hp)
    addr = 0
    regions = []
    m = MBI()
    while k.VirtualQueryEx(hp, addr, ctypes.byref(m), ctypes.sizeof(m)):
        if m.State == 0x1000:
            regions.append((m.BaseAddress, m.RegionSize, m.Type, m.Protect, m.AllocationBase))
        addr = m.BaseAddress + m.RegionSize
        if addr >= 0x7fffffffffff:
            break
    stacks = sorted((t["limit"], t["base"], t["name"]) for t in th if t["base"])
    cats = collections.defaultdict(lambda: collections.Counter())
    groups = collections.defaultdict(lambda: collections.Counter())
    images = []
    for base, size, typ, prot, alloc in regions:
        n = size // PAGE
        arr = (WSEX * n)()
        for i in range(n):
            arr[i].VirtualAddress = base + i * PAGE
        psapi.QueryWorkingSetEx(hp, arr, ctypes.sizeof(arr))
        name = ""
        if typ in (0x1000000, 0x40000):
            buf = ctypes.create_unicode_buffer(600)
            psapi.GetMappedFileNameW(hp, base, buf, 600)
            name = buf.value.rsplit("\\", 1)[-1] or "(section-anon)"
        if typ == 0x1000000:
            cat = "image:" + name
        elif typ == 0x40000:
            cat = "mapped:" + name
        else:
            cat = None
            for lo, hi, nm in stacks:
                if lo <= base < hi or (lo - 0x200000 < base and base < hi and alloc <= lo):
                    cat = "stack"
                    break
            if cat is None:
                x = prot & 0xff
                cat = "private-exec" if x in (0x10, 0x20, 0x40, 0x80) else "private-data"
        priv = shared = 0
        for e in arr:
            if e.Attr & 1:
                if (e.Attr >> 15) & 1:
                    shared += PAGE
                else:
                    priv += PAGE
        if typ == 0x1000000:
            images.append((base, size, name, alloc))
        if typ == 0x20000:
            g = groups[alloc]
            g["pws"] += priv
            g["committed"] += size
            g["regions"] += 1
            g["exec"] += size if (prot & 0xff) in (0x10, 0x20, 0x40, 0x80) else 0
        c = cats[cat]
        c["committed"] += size
        c["pws"] += priv
        c["sws"] += shared
        c["regions"] += 1
    totals = collections.Counter()
    for c in cats.values():
        totals.update(c)
    starts = collections.Counter()
    for t2 in th:
        s = t2.get("start", 0)
        lbl = "?"
        for b, sz, nm, al in images:
            if b <= s < b + sz:
                lbl = nm + "+" + hex(s - al)
                break
        starts[(t2["name"], lbl)] += 1
    grp = sorted(({"base": hex(a), **dict(g)} for a, g in groups.items()), key=lambda x: -x["pws"])[:120]
    for x in grp:
        buf = (ctypes.c_uint64 * 8)()
        got = ctypes.c_size_t()
        k.ReadProcessMemory(hp, int(x["base"], 16), buf, 64, ctypes.byref(got))
        x["head"] = [hex(v) for v in buf]
    res2 = []
    imgs = sorted(images)
    def where(a):
        for b, sz, nm, al in imgs:
            if b <= a < b + sz:
                return nm, a - al
        return None
    for t3 in th:
        if t3["name"] or not t3.get("start"):
            continue
        hi = t3["base"]
        n = 0x3000
        bb = (ctypes.c_uint64 * (n // 8))()
        got = ctypes.c_size_t()
        k.ReadProcessMemory(hp, hi - n, bb, n, ctypes.byref(got))
        calls = []
        for v in reversed(list(bb)):
            w = where(v)
            if w:
                calls.append(w[0] + "+" + hex(w[1]))
                if len(calls) >= 8:
                    break
        res2.append(calls)
    thread_names = collections.Counter()
    for t in th:
        thread_names[t["name"]] += 1
    k.CloseHandle(hp)
    return {"pid": pid, "totals": dict(totals), "cats": {a: dict(b) for a, b in cats.items()},
            "threads": len(th), "thread_names": dict(thread_names), "thread_starts": [[a, b, n] for (a, b), n in starts.most_common()], "groups": grp, "thread_stacks": res2}


out = {}
for p in sys.argv[2:]:
    r = scan(int(p))
    if r:
        out[p] = r
open(sys.argv[1], "w").write(json.dumps(out))
for p, r in out.items():
    print(p, "pws", r["totals"]["pws"] // 1048576, "MB sws", r["totals"]["sws"] // 1048576, "MB commit", r["totals"]["committed"] // 1048576, "MB threads", r["threads"])
