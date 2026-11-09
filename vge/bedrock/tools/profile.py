import argparse
import collections
import json
import re
import subprocess
from pathlib import Path

SYMBOLIZER = Path("C:/Users/pavel/.mozbuild/clang/bin/llvm-symbolizer.exe")
BIN = Path("C:/Projects/gecko-obj-bedrock/dist/bin")


def symbolize(addresses, libs):
    by_lib = collections.defaultdict(list)
    names = {}
    for address in addresses:
        lib = next((l for l in libs if l["start"] <= address < l["end"]), None)
        if not lib:
            names[address] = hex(address)
            continue
        by_lib[lib["name"]].append((address, address - lib["start"], lib))
    for name, items in by_lib.items():
        path = Path(items[0][2].get("path") or "")
        if BIN.joinpath(name).exists():
            path = BIN / name
        if not path.exists():
            for address, offset, _ in items:
                names[address] = f"{name}+{offset:#x}"
            continue
        stdin = "\n".join(hex(offset + 0x180000000) for _, offset, _ in items) + "\n"
        out = subprocess.run(
            [str(SYMBOLIZER), f"--obj={path}", "--functions=linkage", "--no-inlines", "--demangle"],
            input=stdin,
            capture_output=True,
            text=True,
        ).stdout.splitlines()
        results = [out[i] for i in range(0, len(out), 3)] if out else []
        for (address, offset, _), result in zip(items, results):
            names[address] = (
                f"{name}+{offset:#x}" if result == "??" else f"{name}!{result}"
            )
        for address, offset, _ in items:
            names.setdefault(address, f"{name}+{offset:#x}")
    return names


def thread_table(thread):
    samples = thread["samples"]
    schema = samples["schema"]
    frames = thread["frameTable"]
    stacks = thread["stackTable"]
    return (
        samples["data"],
        schema,
        frames["data"],
        frames["schema"],
        stacks["data"],
        stacks["schema"],
        thread["stringTable"],
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("profile")
    parser.add_argument("--thread", default="GeckoMain")
    parser.add_argument("--callers")
    parser.add_argument("--top", type=int, default=40)
    parser.add_argument("--from-ms", type=float, default=0)
    parser.add_argument("--to-ms", type=float, default=1e12)
    args = parser.parse_args()

    data = json.load(open(args.profile, encoding="utf-8"))
    threads = [t for t in data["threads"] if t["name"] == args.thread]
    thread = threads[0]
    print("threads", collections.Counter(t["name"] for t in data["threads"]).most_common(12))
    samples, s_schema, frames, f_schema, stacks, st_schema, strings = thread_table(thread)
    libs = data["libs"]

    locations = {}
    for index, frame in enumerate(frames):
        locations[index] = strings[frame[f_schema["location"]]]

    def native(location):
        match = re.fullmatch(r"0x[0-9a-f]+", location)
        return int(location, 16) if match else None

    addresses = {native(v) for v in locations.values() if native(v) is not None}
    symbols = symbolize(addresses, libs)

    def label(frame):
        location = locations[frame]
        address = native(location)
        return symbols.get(address, location) if address is not None else location

    callers = collections.Counter()
    self_counts = collections.Counter()
    total_counts = collections.Counter()
    count = 0
    first_time = samples[0][s_schema["time"]] if samples else 0
    for sample in samples:
        relative = sample[s_schema["time"]] - first_time
        if not args.from_ms <= relative <= args.to_ms:
            continue
        stack = sample[s_schema["stack"]]
        if stack is None:
            continue
        count += 1
        if args.callers:
            chain = []
            walk = stack
            while walk is not None:
                chain.append(label(stacks[walk][st_schema["frame"]]))
                walk = stacks[walk][st_schema["prefix"]]
            for i, name in enumerate(chain):
                if args.callers in name:
                    callers[" < ".join(c[:70] for c in chain[i + 1 : i + 6])] += 1
                    break
        seen = set()
        leaf = True
        while stack is not None:
            frame = stacks[stack][st_schema["frame"]]
            name = label(frame)
            if leaf:
                self_counts[name] += 1
                leaf = False
            if name not in seen:
                total_counts[name] += 1
                seen.add(name)
            stack = stacks[stack][st_schema["prefix"]]
    print("samples", count, "span_ms", round(samples[-1][s_schema["time"]] - first_time))
    if args.callers:
        print("--- callers of", args.callers)
        for name, value in callers.most_common(args.top):
            print(value, name[:400])
        return
    print("--- self")
    for name, value in self_counts.most_common(args.top):
        print(value, name[:130])
    print("--- inclusive")
    for name, value in total_counts.most_common(args.top):
        print(value, name[:130])


if __name__ == "__main__":
    main()
