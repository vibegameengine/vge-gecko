import argparse
import json
import os
import re
import subprocess
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
PROBE = HERE.parent / "test" / "frame-probe.js"
HOST = Path("C:/Projects/gecko-obj-bedrock/dist/bin/VgeBedrockTest.exe")
RESULTS = Path("C:/Projects/gecko-build-logs/bench")
GAME = "https://claybound-56949.web.app/"


def slot(action, name):
    script = Path.home() / ".claude/skills/browser-slots/browser-slots.mjs"
    args = ["node", str(script), action, name]
    if action == "acquire-perf":
        args += ["--yes-timing", "bench.py frame and memory run"]
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL)


def gpu_memory(pid):
    command = (
        f"$c = Get-Counter '\\GPU Process Memory(pid_{pid}_*)\\Dedicated Usage' "
        "-ErrorAction SilentlyContinue; "
        "[int](($c.CounterSamples | Measure-Object CookedValue -Sum).Sum / 1MB)"
    )
    out = subprocess.run(
        ["powershell", "-NoProfile", "-Command", command],
        capture_output=True,
        text=True,
    ).stdout.strip()
    return int(out) if out.isdigit() else None


def working_set(pid, destination):
    out = subprocess.run(
        ["python", "-I", str(HERE / "regions.py"), str(destination), str(pid)],
        capture_output=True,
        text=True,
    ).stdout
    match = re.search(r"pws (\d+) MB sws (\d+) MB commit (\d+) MB", out)
    if not match:
        return None
    return {
        "private_ws_mb": int(match[1]),
        "shared_ws_mb": int(match[2]),
        "commit_mb": int(match[3]),
    }


def parse_frames(text):
    lines = re.findall(r"frameprobe fps (\d+) longest ([\d.]+) over25 (\d+) over50 (\d+)", text)
    samples = [(int(a), float(b), int(c), int(d)) for a, b, c, d in lines]
    if not samples:
        return {"samples": 0}
    first = max(samples, key=lambda s: s[1])
    steady = [s for s in samples if s is not first]
    longest = [s[1] for s in steady] or [0]
    return {
        "samples": len(samples),
        "first_frame_ms": first[1],
        "fps_min": min((s[0] for s in steady), default=0),
        "longest_max_ms": max(longest),
        "longest_median_ms": sorted(longest)[len(longest) // 2],
        "over25_per_sample": round(sum(s[2] for s in steady) / max(len(steady), 1), 2),
        "over50_per_sample": round(sum(s[3] for s in steady) / max(len(steady), 1), 2),
    }


def parse_gc(text):
    pauses = []
    for line in text.splitlines():
        if "mark=" not in line or "pause=" not in line:
            continue
        fields = dict(re.findall(r"([\w.]+)=([\d.]+)", line))
        if float(fields.get("mark", 0)) >= 1:
            pauses.append(float(fields["pause"]))
    pauses.sort()
    if not pauses:
        return {"mark_compacts": 0}
    return {
        "mark_compacts": len(pauses),
        "pause_median_ms": pauses[len(pauses) // 2],
        "pause_max_ms": pauses[-1],
    }


def parse_gray(text):
    rows = re.findall(
        r"VGEGRAY closure ([\d.]+) sort ([\d.]+) edges ([\d.]+) weak ([\d.]+) client ([\d.]+)",
        text,
    )
    if not rows:
        return None
    worst = max(rows, key=lambda r: sum(float(v) for v in r))
    return dict(zip(("closure", "sort", "edges", "weak", "client"), map(float, worst)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("label")
    parser.add_argument("--url", default=GAME)
    parser.add_argument("--seconds", type=int, default=75)
    parser.add_argument("--trace", action="store_true")
    parser.add_argument("--profile", action="store_true")
    parser.add_argument("--gc", action="store_true")
    parser.add_argument("--alloc", action="store_true")
    parser.add_argument("--compare")
    args = parser.parse_args()

    RESULTS.mkdir(parents=True, exist_ok=True)
    log_path = RESULTS / f"{args.label}.log"
    env = dict(os.environ)
    env["VGE_FRAME_PROBE"] = str(PROBE).replace("/", "\\")
    if args.gc:
        env["VGE_V8_FLAGS"] = "--trace-gc-nvp"
    trace_path = RESULTS / f"{args.label}.trace.txt"
    if args.profile:
        env["MOZ_PROFILER_STARTUP"] = "1"
        env["MOZ_PROFILER_STARTUP_ENTRIES"] = "4000000"
        env["MOZ_PROFILER_STARTUP_INTERVAL"] = "4"
        env["MOZ_PROFILER_STARTUP_FEATURES"] = "stackwalk,nativeallocations" if args.alloc else "stackwalk"
        env["MOZ_PROFILER_STARTUP_FILTERS"] = "GeckoMain,Compositor,Renderer,WRRenderBackend"
        env["VGE_PROFILE"] = str(RESULTS / f"{args.label}.profile.json").replace("/", "\\")
        env["VGE_PROFILE_DELAY"] = str(max(args.seconds - 45, 5))
    if args.trace:
        env["VGE_ALLOC_TRACE"] = str(trace_path).replace("/", "\\")
        env["VGE_ALLOC_TRACE_DELAY"] = str(max(args.seconds - 15, 5))

    subprocess.run(
        ["taskkill", "/F", "/IM", HOST.name],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    slot("acquire-perf", f"bench-{args.label}")
    try:
        with open(log_path, "w", encoding="utf-8", errors="replace") as log:
            process = subprocess.Popen(
                [str(HOST), "--window", args.url],
                cwd=HOST.parent,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
            )
            time.sleep(args.seconds)
            crashed = process.poll() is not None
            if args.profile:
                profile = RESULTS / f"{args.label}.profile.json"
                deadline = time.time() + 90
                while time.time() < deadline and process.poll() is None:
                    try:
                        json.loads(profile.read_text(encoding="utf-8"))
                        break
                    except (OSError, ValueError):
                        time.sleep(3)
                else:
                    if process.poll() is None:
                        cdb = "C:/Users/pavel/.mozbuild/vs/Windows Kits/10/Debuggers/x64/cdb.exe"
                        out = subprocess.run(
                            [cdb, "-y", str(HOST.parent), "-p", str(process.pid), "-c", "~*kc 24; .detach; q"],
                            capture_output=True,
                            text=True,
                        ).stdout
                        (RESULTS / f"{args.label}.stacks.txt").write_text(out)
            memory = None if crashed else working_set(process.pid, RESULTS / f"{args.label}.regions.json")
            video = None if crashed else gpu_memory(process.pid)
            process.kill()
            process.wait()
    finally:
        slot("release", f"bench-{args.label}")

    text = log_path.read_text(encoding="utf-8", errors="replace")
    result = {
        "label": args.label,
        "crashed": crashed,
        "memory": memory,
        "video_mb": video,
        "frames": parse_frames(text),
        "gc": parse_gc(text),
        "gray_worst_ms": parse_gray(text),
    }
    (RESULTS / f"{args.label}.json").write_text(json.dumps(result, indent=1))
    print(json.dumps(result))
    if args.compare:
        before = json.loads((RESULTS / f"{args.compare}.json").read_text())
        print("before", json.dumps(before))


if __name__ == "__main__":
    main()
